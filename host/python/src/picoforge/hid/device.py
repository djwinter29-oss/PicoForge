"""HIDAPI transport for PicoForge's vendor-defined status interface."""

from __future__ import annotations

from math import ceil
from functools import wraps
from threading import RLock
from time import monotonic, sleep
from typing import Any, Callable

from .models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    InputReport,
    MscStatus,
    RuntimeStatus,
    WirelessNetwork,
    WirelessOperation,
    WirelessSaveResult,
    WirelessStatus,
)
from ..reports import (
    CAP_FORCE_SAVE,
    CAP_WIRELESS,
    WIRELESS_REPORT_ID,
    WIRELESS_FEATURE_SIZE,
    build_tcp_cli_auth_request,
    validate_wireless_configuration,
    build_wireless_command_report,
    parse_wireless_status_report,
    CAP_LED,
    CAP_REBOOT,
    CAP_SAVE_SETTINGS,
    CAP_SELF_TEST,
    CONFIGURATION_FEATURE_SIZE,
    CONFIGURATION_REPORT_ID,
    CONTROL_FEATURE_SIZE,
    CONTROL_REPORT_ID,
    IDENTITY_FEATURE_SIZE,
    IDENTITY_REPORT_ID,
    STORAGE_INPUT_SIZE,
    STORAGE_REPORT_ID,
    TELEMETRY_INPUT_SIZE,
    TELEMETRY_REPORT_ID,
    SELF_TEST_REPORT_ID,
    SELF_TEST_INPUT_SIZE,
    build_control_command_report,
    build_settings_command_report,
    build_led_command_report,
    build_msc_command_report,
    build_self_test_command_report,
    parse_identity_report,
    parse_storage_report,
    parse_configuration_report,
    parse_control_result_report,
    parse_self_test_status_report,
    parse_telemetry_report,
)
from ..reports.common import _validate_settings
from ..reports.wireless import display_ssid, request_ssid

VENDOR_ID = 0xCAFE
PRODUCT_ID = 0x4011
PRODUCT_IDS = {0x4011, 0x4012}
USAGE_PAGE = 0xFF00
USAGE = 0x0001
PRODUCT_STRING = "PicoForge"

try:
    import hid
except ImportError as error:  # Import only when a hardware connection is requested.
    hid = None
    HID_IMPORT_ERROR = error
else:
    HID_IMPORT_ERROR = None


def _require_hid() -> Any:
    if hid is None:
        raise RuntimeError("HIDAPI is missing; install host/python with 'pip install -e host/python'") from HID_IMPORT_ERROR
    return hid


def _path_string(path: Any) -> str:
    if isinstance(path, bytes):
        return path.decode("utf-8", errors="surrogateescape")
    return str(path or "")


def _matches_vendor_collection(info: dict[str, Any]) -> bool:
    usage_page = info.get("usage_page")
    usage = info.get("usage")
    if usage_page not in (None, 0) and usage not in (None, 0):
        return usage_page == USAGE_PAGE and usage == USAGE
    product_string = info.get("product_string")
    if product_string:
        return product_string == PRODUCT_STRING
    # enumerate() is already restricted to our VID/PID; some HIDAPI backends
    # leave usage and string metadata empty when the device is not readable.
    return True


def _matching_devices() -> list[dict[str, Any]]:
    devices = _require_hid().enumerate(VENDOR_ID, 0)
    return [info for info in devices
            if info.get("product_id") in PRODUCT_IDS and _matches_vendor_collection(info)]


def enumerate_devices() -> list[dict[str, str | None]]:
    """Return board identity metadata without opening a HID connection."""
    return [{
        "product_name": info.get("product_string") or "PicoForge",
        "serial_number": info.get("serial_number") or None,
        "device_path": _path_string(info.get("path")),
    } for info in _matching_devices()]


def _select_device(serial_number: str | None, device_path: str | None) -> dict[str, Any]:
    if serial_number is not None and device_path is not None:
        raise ValueError("--serial and --device-path cannot be used together")
    matches = _matching_devices()
    if serial_number is not None:
        matches = [info for info in matches if info.get("serial_number") == serial_number]
    if device_path is not None:
        matches = [info for info in matches if _path_string(info.get("path")) == device_path]
    if not matches:
        raise ConnectionError("PicoForge HID interface not found; check the USB connection and HID permissions")
    if len(matches) != 1:
        raise RuntimeError("multiple PicoForge HID interfaces matched; select one with --serial or --device-path")
    return matches[0]


def _serialized(method):
    """Keep the result slot and multi-command wireless lifecycles owned by one caller."""
    @wraps(method)
    def call(self, *args, **kwargs):
        with self._control_lock:
            return method(self, *args, **kwargs)
    return call


class PicoForgeHid:
    """Own one PicoForge HID connection and expose decoded reports."""

    def __init__(self, serial_number: str | None = None, device_path: str | None = None) -> None:
        info = _select_device(serial_number, device_path)
        device = _require_hid().device()
        try:
            device.open_path(info["path"])
        except OSError as error:
            raise RuntimeError(
                "failed to open PicoForge HID interface; check device permissions "
                "(Linux setup: see host/python/README.md)"
            ) from error
        self._device = device
        self.closed = False
        self._expected_serial_number = info.get("serial_number")
        self.identity: HardwareIdentity | None = None
        self.configuration: BoardConfiguration | None = None
        self._transaction_id = 0
        self.last_command_result: dict[str, object] | None = None
        self._command_uncertain = False
        self._control_lock = RLock()
        self._wireless_snapshot: WirelessStatus | None = None
        self._wireless_active = False

    @property
    def command_uncertain(self) -> bool:
        """True after a write whose firmware outcome could not be confirmed; further writes are blocked."""
        return self._command_uncertain

    @property
    def wireless_active(self) -> bool:
        """True while the last Feature 12 read showed a scan or connection test in progress."""
        return self._wireless_active

    def read_info(self) -> HardwareIdentity:
        self._require_open()
        report = self._device.get_feature_report(IDENTITY_REPORT_ID, IDENTITY_FEATURE_SIZE)
        self.identity = parse_identity_report(report)
        if (self._expected_serial_number and self.identity.get("supported")
                and str(self._expected_serial_number).upper() != self.identity["serial_number"]):
            raise ValueError("HID identity UID disagrees with the USB serial string")
        return self.identity

    def read_configuration(self) -> BoardConfiguration:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity.get("supported"):
            raise RuntimeError(f"unsupported product: {identity['product_name']}")
        report = self._device.get_feature_report(CONFIGURATION_REPORT_ID, CONFIGURATION_FEATURE_SIZE)
        self.configuration = parse_configuration_report(report, identity)
        return self.configuration

    def read_settings(self) -> dict[str, int]:
        configuration = self.read_configuration()
        return {
            "system_clock_mhz": configuration["system_clock_mhz"],
            "sdcard_profile": configuration["sdcard_profile"],
            "radios": configuration["radios"],
            "tcp_cli_enabled": configuration["tcp_cli_enabled"],
        }

    def read_status(self, timeout_ms: int = 250) -> RuntimeStatus | None:
        """Consume non-status input reports while waiting up to timeout_ms for status."""
        if timeout_ms < 0:
            raise ValueError("timeout_ms must not be negative")
        deadline = monotonic() + timeout_ms / 1000
        remaining_ms = timeout_ms
        while True:
            item = self.read_input_report(remaining_ms)
            if item is None:
                return None
            if item["kind"] == "runtime":
                return item["data"]
            remaining_ms = ceil((deadline - monotonic()) * 1000)
            if remaining_ms <= 0:
                return None

    def read_input_report(self, timeout_ms: int = 250) -> InputReport | None:
        if timeout_ms < 0:
            raise ValueError("timeout_ms must not be negative")
        self._require_open()
        report = self._device.read(
            max(TELEMETRY_INPUT_SIZE, STORAGE_INPUT_SIZE, SELF_TEST_INPUT_SIZE), timeout_ms
        )
        if not report:
            return None
        identity = self.identity or self.read_info()
        if not identity.get("supported"):
            raise RuntimeError(f"unsupported product: {identity['product_name']}")
        if report[0] == STORAGE_REPORT_ID:
            data = parse_storage_report(report, identity)
            return {"kind": "memory", "data": data}
        if report[0] == SELF_TEST_REPORT_ID:
            if not identity["self_test_supported"]:
                raise ValueError("device published self-test status without advertising capability bit 5")
            return {"kind": "self_test", "data": parse_self_test_status_report(report)}
        if report[0] == TELEMETRY_REPORT_ID:
            return {"kind": "runtime", "data": parse_telemetry_report(report, identity)}
        raise ValueError(f"unexpected HID Input report ID {report[0]}")

    @_serialized
    def set_led(self, on: bool) -> ControlResult:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_LED:
            raise RuntimeError("board identification LED is unavailable")
        return self._execute_control(build_led_command_report(on, self._next_transaction_id()), 1)

    @_serialized
    def set_settings(self, settings: dict[str, object], *, force: bool = False) -> BootSettings:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_SAVE_SETTINGS:
            raise RuntimeError("persistent settings are unavailable on this firmware")
        if force and not identity["capabilities"] & CAP_FORCE_SAVE:
            raise RuntimeError("forced settings save is unavailable on this firmware")
        settings = self._complete_settings(settings)
        command = build_settings_command_report(settings, self._next_transaction_id(), force=force)
        result = self._execute_control(command, 5, reconnect=force)
        if result["status"] != "completed":
            raise RuntimeError(f"settings command {result['status']}: {result['error']}")
        saved = result["saved_settings"]
        if saved != settings:
            self._command_uncertain = True
            raise RuntimeError("firmware did not confirm the selected boot settings")
        return saved

    def _complete_settings(self, settings: dict[str, object]) -> dict[str, object]:
        """Resolve omitted service switches only from the authoritative persisted configuration."""
        _validate_settings(settings)
        if "tcp_cli_enabled" in settings:
            return dict(settings)
        configuration = self.read_configuration()
        return {**settings, "tcp_cli_enabled": configuration["tcp_cli_enabled"]}

    @_serialized
    def reboot(self) -> ControlResult:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_REBOOT:
            raise RuntimeError("standalone reboot is unavailable on this firmware")
        command = build_control_command_report(self._next_transaction_id(), 2, flags=1)
        return self._execute_control(command, 2)

    def read_wireless(self) -> WirelessStatus:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity.get("capabilities", 0) & CAP_WIRELESS:
            raise RuntimeError("Wireless provisioning is unavailable on this firmware")
        try:
            status = parse_wireless_status_report(
                self._device.get_feature_report(WIRELESS_REPORT_ID, WIRELESS_FEATURE_SIZE))
        except (OSError, ValueError):
            cached = getattr(self, "_wireless_snapshot", None)
            if cached is not None:
                cached["stale"] = True
            raise
        configuration = getattr(self, "configuration", None) or self.read_configuration()
        status.update(wifi_enabled=bool(configuration["radios"] & 1),
                      ble_enabled=bool(configuration["radios"] & 2),
                      tcp_cli_enabled=configuration["tcp_cli_enabled"], stale=False, age_ms=0)
        if status["pair_tested"] and status["test_generation"] == getattr(self, "_tested_generation", None):
            status["remaining_seconds"] = max(0, 600 - ceil(monotonic() - self._tested_at))
        self._wireless_snapshot = status
        self._wireless_active = status["wifi_state_id"] == 4 or status["scan_state"] == "scanning"
        return status

    @_serialized
    def tcp_cli_auth_proof(self, nonce: bytes, uid: bytes) -> bytes:
        """Ask this board for the fixed TCP CLI proof; never expose or copy its saved key."""
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity.get("capabilities", 0) & CAP_WIRELESS:
            raise RuntimeError("USB TCP CLI authentication is unavailable on this firmware")
        try:
            expected_uid = bytes.fromhex(identity["serial_number"])
        except (KeyError, ValueError) as error:
            raise RuntimeError("The connected board identity is unavailable") from error
        if not isinstance(uid, bytes) or len(uid) != 8 or uid != expected_uid:
            raise ValueError("TCP CLI challenge belongs to a different board")
        result = self._execute_control(build_tcp_cli_auth_request(self._next_transaction_id(), nonce), 13,
                                       retry_get=True)
        self._require_completed(result)
        return bytes(result["proof"])

    @_serialized
    def set_wireless(self, settings: dict[str, object]) -> WirelessSaveResult:
        """Save board settings and credentials atomically; return wireless status plus confirmed settings."""
        self._require_open()
        if self._command_uncertain:
            raise RuntimeError("A prior command outcome is uncertain; further writes are blocked")
        validate_wireless_configuration(settings)
        previous = self.read_wireless()
        board = dict(settings["board"])
        if "tcp_cli_enabled" in settings:
            board["tcp_cli_enabled"] = settings["tcp_cli_enabled"]
        board = self._complete_settings(board)
        use_tested = settings.get("use_tested_wifi", False)
        if use_tested:
            if (not previous["pair_tested"] or previous["staged_state"] & 8
                    or (("ssid" in settings or "ssid_hex" in settings)
                        and request_ssid(settings).hex() != previous["ssid_hex"])):
                raise RuntimeError("The exact staged Wi-Fi pair has not passed testing or has expired")
        else:
            for field, opcode in (("ssid", 6), ("password", 7)):
                if field in settings or (field == "ssid" and "ssid_hex" in settings):
                    self._wireless_command(opcode, value=request_ssid(settings) if field == "ssid" else settings[field])
        if "ble_key" in settings:
            self._wireless_command(8, value=settings["ble_key"])
        staged = self.read_wireless()
        if use_tested and (not staged["pair_tested"] or staged["test_generation"] != previous["test_generation"]):
            raise RuntimeError("The tested Wi-Fi pair changed before saving")
        commit = staged["ssid_staged"] or staged["password_staged"] or staged["key_staged"]
        expected_mask = (int(staged["ssid_staged"]) | (int(staged["password_staged"]) << 1)
                         | (int(staged["key_staged"]) << 2))
        command = build_settings_command_report(board, self._next_transaction_id(), commit=commit)
        result = self._execute_control(command, 5)
        self._require_completed(result)
        if result["saved_settings"] != board or result["commit_mask"] != expected_mask:
            self._command_uncertain = True
            raise RuntimeError("Saved board settings or credential commit were not confirmed; outcome is uncertain")
        # Reset is already scheduled. Confirm saved fields from the verified commit, not a post-save GET.
        response = {**staged, "wifi_enabled": bool(board["radios"] & 1),
                    "ble_enabled": bool(board["radios"] & 2), "tcp_cli_enabled": board["tcp_cli_enabled"],
                    "stale": True, "settings": result["saved_settings"]}
        if not use_tested and ("ssid" in settings or "ssid_hex" in settings):
            raw = request_ssid(settings)
            response.update(ssid=display_ssid(raw), ssid_hex=raw.hex(),
                            network_configured=bool(raw))
        if "ble_key" in settings:
            response["ble_key_configured"] = bool(settings["ble_key"])
        return response

    @_serialized
    def scan_wireless(self, *, on_poll: Callable[[], None] | None = None) -> list[WirelessNetwork]:
        """Scan for nearby access points and return strongest unique SSIDs."""
        self._require_open()
        previous = self.read_wireless()
        self._wireless_command(11)
        scan = self._poll_wireless("scan", previous["scan_generation"], 15.0, on_poll=on_poll)
        networks = []
        for index in range(scan["scan_count"]):
            result = self._wireless_command(12, index=index, generation=scan["scan_generation"])
            entry = result["scan_entry"]
            if entry["generation"] != scan["scan_generation"] or entry["index"] != index:
                self._command_uncertain = True
                raise ValueError("Mismatched scan entry generation or index")
            networks.append(entry)
        return networks

    @_serialized
    def test_wireless(
        self, ssid: str, password: str = "", *, on_poll: Callable[[], None] | None = None,
        on_progress: Callable[[str], None] | None = None,
    ) -> WirelessOperation:
        """Test a temporary network join; firmware retains credentials only in RAM after success."""
        self._require_open()
        # Validate both fields before staging either, including opaque SSIDs selected from scans.
        build_wireless_command_report(1, 6, value=ssid)
        build_wireless_command_report(1, 7, value=password)
        previous = self.read_wireless()
        self._wireless_command(6, value=ssid)
        self._wireless_command(7, value=password)
        result = self._wireless_command(9)
        status = self._poll_wireless("test", previous["test_generation"], 35.0,
                                     on_poll=on_poll, on_progress=on_progress)
        self._tested_generation = status["test_generation"]
        self._tested_at = monotonic()
        return {"transaction_id": result["transaction_id"], "state": 5, "page": 0,
                "detail": status["detail"], "phase": status["phase"], "count": status["scan_count"],
                "wifi_state": status["wifi_state_id"], "ipv4": status["ipv4"], "networks": [],
                "test_generation": status["test_generation"], "scan_generation": status["scan_generation"],
                "remaining_seconds": 600}

    @_serialized
    def cancel_wireless_test(self) -> None:
        """Clear temporary test credentials from firmware RAM."""
        self._require_open()
        self._wireless_command(10)

    def _wireless_command(
        self, operation: int, *, value: object = None, index: int = 0, generation: int = 0,
    ) -> ControlResult:
        identity = self.identity or self.read_info()
        if not identity.get("capabilities", 0) & CAP_WIRELESS:
            raise RuntimeError("Wireless provisioning is unavailable on this firmware")
        command = build_wireless_command_report(self._next_transaction_id(), operation, value=value,
                                                index=index, generation=generation)
        result = self._execute_control(command, operation)
        self._require_completed(result)
        return result

    @staticmethod
    def _require_completed(result: ControlResult) -> None:
        if result["status"] != "completed":
            raise RuntimeError(f"Wireless command {result['status']}: {result['error']} (detail {result['detail']})")

    def _poll_wireless(
        self, kind: str, previous_generation: int, timeout: float, *,
        on_poll: Callable[[], None] | None = None, on_progress: Callable[[str], None] | None = None,
    ) -> WirelessStatus:
        deadline = monotonic() + timeout
        modulus = 65535 if kind == "scan" else 255
        expected_generation = previous_generation % modulus + 1
        self._command_uncertain = True
        while monotonic() < deadline:
            if on_poll is not None:
                on_poll()
            status = self.read_wireless()
            generation = status[f"{kind}_generation"]
            if generation not in (previous_generation, expected_generation):
                raise ValueError("Wireless operation generation changed unexpectedly; outcome is uncertain")
            if generation == expected_generation:
                failed = status["scan_state"] == "failed" if kind == "scan" else status["wifi_state_id"] == 6
                completed = status["scan_state"] == "complete" if kind == "scan" else (
                    status["wifi_state_id"] == 5 and status["pair_tested"])
                if failed or completed:
                    self._command_uncertain = False
                    if failed:
                        raise RuntimeError(f"Wireless {kind} failed: {status['phase'] or 'Timed out'}")
                    return status
                if on_progress is not None:
                    on_progress(status["phase"] or "Connecting to Wi-Fi")
            sleep(0.5)
        raise TimeoutError("Wireless operation timed out; do not resend until its result is checked")

    @_serialized
    def set_msc(self, enable: bool, *, force: bool = False) -> ControlResult:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & (1 << 3 | 1 << 4):
            raise RuntimeError("MSC control is unavailable without a selected storage transport")
        return self._execute_control(
            build_msc_command_report(enable, force=force, transaction_id=self._next_transaction_id()), 3
        )

    def close(self) -> None:
        if not self.closed:
            self._device.close()
            self.closed = True
            self._wireless_snapshot = None
    
    @_serialized
    def set_self_test(self, action: str, size_mib: int | None = None) -> ControlResult:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_SELF_TEST:
            raise RuntimeError("SD self-test is unavailable on this firmware")
        command = build_self_test_command_report(action, size_mib, self._next_transaction_id())
        return self._execute_control(command, 4)

    def _next_transaction_id(self) -> int:
        self._transaction_id = self._transaction_id % 255 + 1
        return self._transaction_id

    def _execute_control(
        self, command: bytes, opcode: int, *, reconnect: bool = False, retry_get: bool = False,
    ) -> ControlResult:
        if self._command_uncertain:
            raise RuntimeError("a prior HID command has an uncertain outcome; reconnect and inspect device state")
        self._command_uncertain = True
        sent = self._device.send_feature_report(list(command))
        if sent != len(command):
            raise OSError(f"failed to send HID control opcode {opcode}")
        transaction_id = command[2]
        deadline = monotonic() + 5.0
        while True:
            try:
                report = self._device.get_feature_report(CONTROL_REPORT_ID, CONTROL_FEATURE_SIZE)
                result = parse_control_result_report(report, self.identity)
            except OSError as error:
                if retry_get and monotonic() < deadline:
                    sleep(0.05)
                    continue
                if reconnect and monotonic() < deadline:
                    self._command_uncertain = True
                    self._reopen_for_result(deadline, error)
                    continue
                self._command_uncertain = True
                raise
            except ValueError:
                self._command_uncertain = True
                raise
            if result["transaction_id"] == transaction_id and result["opcode"] == opcode:
                if result["status"] != "pending":
                    self._command_uncertain = False
                    # Proof bytes are not dashboard snapshot data.
                    self.last_command_result = {**result, "proof": None, "result_data": (
                        0 if opcode == 13 else result["result_data"])}
                    return result
            if monotonic() >= deadline:
                self._command_uncertain = True
                raise TimeoutError(
                    f"timed out waiting for HID control transaction {transaction_id}; outcome is uncertain"
                )
            sleep(0.05)

    def _reopen_for_result(self, deadline: float, disconnect_error: OSError) -> None:
        """Reopen only the original UID to read its outstanding result; never replay SET."""
        expected_uid = self.identity["serial_number"]
        self._device.close()
        self.closed = True
        while monotonic() < deadline:
            sleep(0.05)
            candidates = [info for info in _matching_devices()
                          if str(info.get("serial_number", "")).upper() == expected_uid]
            if not candidates:
                continue
            if len(candidates) != 1:
                self._command_uncertain = True
                raise RuntimeError("Multiple HID devices matched the original board UID")
            device = _require_hid().device()
            try:
                device.open_path(candidates[0]["path"])
                identity = parse_identity_report(
                    device.get_feature_report(IDENTITY_REPORT_ID, IDENTITY_FEATURE_SIZE))
            except OSError:
                device.close()
                continue
            except ValueError:
                device.close()
                self._command_uncertain = True
                raise
            if identity["serial_number"] != expected_uid:
                device.close()
                self._command_uncertain = True
                raise RuntimeError("Reconnected HID identity does not match the original board")
            self._device = device
            self.closed = False
            return
        self._command_uncertain = True
        raise TimeoutError("Forced save result unavailable after USB detach; outcome is uncertain") from disconnect_error

    def _require_open(self) -> None:
        if self.closed:
            raise RuntimeError("PicoForge HID connection is closed")

    def __enter__(self) -> "PicoForgeHid":
        self._require_open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()