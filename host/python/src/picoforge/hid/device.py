"""HIDAPI transport for PicoForge's vendor-defined status interface."""

from __future__ import annotations

from math import ceil
from functools import wraps
from threading import Event, RLock
from time import monotonic, sleep
from typing import Any, Callable, Concatenate, ParamSpec, TypeVar

from ..reports.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    InputReport,
    SupportedHardwareIdentity,
    WirelessNetwork,
    WirelessReportStatus,
    WirelessSaveResult,
    WirelessStatus,
    WirelessTestResult,
)
from ..reports import (
    CAP_FORCE_SAVE,
    CAP_WIRELESS,
    ControlOpcode,
    ProtocolError,
    WIRELESS_REPORT_ID,
    WIRELESS_FEATURE_SIZE,
    build_tcp_cli_auth_request,
    validate_wireless_configuration,
    build_wireless_command_report,
    parse_wireless_status_report,
    CAP_LED,
    CAP_REBOOT,
    CAP_SAVE_SETTINGS,
    CAP_SPI,
    CAP_SDIO,
    WifiState,
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
    parse_memory_report,
    parse_configuration_report,
    parse_control_result_report,
    parse_self_test_report,
    parse_status_report,
)
from ..reports.common import _validate_settings
from ..reports.wireless import display_ssid, request_ssid

VENDOR_ID = 0xCAFE
PRODUCT_IDS = {0x4011, 0x4012}
USAGE_PAGE = 0xFF00
USAGE = 0x0001
PRODUCT_STRING = "PicoForge"
_P = ParamSpec("_P")
_T = TypeVar("_T")

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


def _parse_report(parser: Callable[_P, _T], *args: _P.args, **kwargs: _P.kwargs) -> _T:
    """Classify malformed device replies separately from host-side ValueErrors."""
    try:
        return parser(*args, **kwargs)
    except ProtocolError:
        raise
    except ValueError as error:
        raise ProtocolError(str(error)) from error


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


class WirelessTestCancelled(RuntimeError):
    """A connection test stopped after this client sent the Wi-Fi cancel command."""


def _serialized(
    method: Callable[Concatenate[PicoForgeHid, _P], _T],
) -> Callable[Concatenate[PicoForgeHid, _P], _T]:
    """Keep the result slot and multi-command wireless lifecycles owned by one caller."""
    @wraps(method)
    def call(self: PicoForgeHid, *args: _P.args, **kwargs: _P.kwargs) -> _T:
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
        self.last_command_result: ControlResult | None = None
        self._command_uncertain = False
        self._uncertain_command: tuple[int, int] | None = None
        self._uncertain_wireless_operation: tuple[str, int] | None = None
        self._control_lock = RLock()
        self._wireless_active = False
        self._tested_generation: int | None = None
        self._tested_at: float | None = None

    @property
    def command_uncertain(self) -> bool:
        """True after a write whose firmware outcome could not be confirmed; further writes are blocked."""
        return self._command_uncertain or self._uncertain_wireless_operation is not None

    @property
    def uncertain_command(self) -> tuple[int, int] | None:
        return self._uncertain_command

    @property
    def uncertain_wireless_operation(self) -> tuple[str, int] | None:
        return self._uncertain_wireless_operation

    @property
    def wireless_active(self) -> bool:
        """True while the last Feature 12 read showed a scan or connection test in progress."""
        return self._wireless_active

    def read_info(self) -> HardwareIdentity:
        self._require_open()
        report = self._device.get_feature_report(IDENTITY_REPORT_ID, IDENTITY_FEATURE_SIZE)
        identity = _parse_report(parse_identity_report, report)
        self.identity = identity
        if self._expected_serial_number and identity.get("supported"):
            serial_number = identity.get("serial_number")
            if serial_number is None:
                raise ProtocolError("HID identity is missing its USB serial number")
            if str(self._expected_serial_number).upper() != serial_number:
                raise ProtocolError("HID identity UID disagrees with the USB serial string")
        return identity

    def read_configuration(self) -> BoardConfiguration:
        self._require_open()
        identity = self._supported_identity()
        report = self._device.get_feature_report(CONFIGURATION_REPORT_ID, CONFIGURATION_FEATURE_SIZE)
        configuration = _parse_report(parse_configuration_report, report, identity)
        self.configuration = configuration
        return configuration

    def read_input_report(self, timeout_ms: int = 250) -> InputReport | None:
        if timeout_ms < 0:
            raise ValueError("timeout_ms must not be negative")
        self._require_open()
        report = self._device.read(
            max(TELEMETRY_INPUT_SIZE, STORAGE_INPUT_SIZE, SELF_TEST_INPUT_SIZE), timeout_ms
        )
        if not report:
            return None
        identity = self._supported_identity()
        if report[0] == STORAGE_REPORT_ID:
            data = _parse_report(parse_memory_report, report, identity)
            return {"kind": "memory", "data": data}
        if report[0] == SELF_TEST_REPORT_ID:
            if not identity["self_test_supported"]:
                raise ProtocolError("device published self-test status without advertising capability bit 5")
            return {"kind": "self_test", "data": _parse_report(parse_self_test_report, report)}
        if report[0] == TELEMETRY_REPORT_ID:
            return {"kind": "runtime", "data": _parse_report(parse_status_report, report, identity)}
        raise ProtocolError(f"unexpected HID Input report ID {report[0]}")

    def _supported_identity(self) -> SupportedHardwareIdentity:
        identity = self.identity or self.read_info()
        if identity["supported"] is not True:
            raise RuntimeError(f"unsupported product: {identity.get('product_name', 'unknown product')}")
        return identity

    def _require(self, capability: int, message: str) -> SupportedHardwareIdentity:
        identity = self._supported_identity()
        if not identity["capabilities"] & capability:
            raise RuntimeError(message)
        return identity

    @_serialized
    def set_led(self, on: bool) -> ControlResult:
        self._require_open()
        self._require(CAP_LED, "board identification LED is unavailable")
        return self._execute_control(
            build_led_command_report(on, self._next_transaction_id()), ControlOpcode.LED)

    @_serialized
    def set_settings(self, settings: dict[str, object], *, force: bool = False) -> BootSettings:
        self._require_open()
        identity = self._require(CAP_SAVE_SETTINGS, "persistent settings are unavailable on this firmware")
        if force and not identity["capabilities"] & CAP_FORCE_SAVE:
            raise RuntimeError("forced settings save is unavailable on this firmware")
        settings = self._complete_settings(settings)
        command = build_settings_command_report(settings, self._next_transaction_id(), force=force)
        result = self._execute_control(command, ControlOpcode.SAVE_SETTINGS, reconnect=force)
        self._require_completed(result, "Settings")
        saved = result["saved_settings"]
        if saved is None or saved != settings:
            raise RuntimeError("Firmware did not confirm the requested settings")
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
        self._require(CAP_REBOOT, "standalone reboot is unavailable on this firmware")
        command = build_control_command_report(self._next_transaction_id(), ControlOpcode.REBOOT, flags=1)
        return self._execute_control(command, ControlOpcode.REBOOT)

    def read_wireless(self) -> WirelessStatus:
        self._require_open()
        self._require(CAP_WIRELESS, "Wireless provisioning is unavailable on this firmware")
        report_status: WirelessReportStatus = _parse_report(
            parse_wireless_status_report,
            self._device.get_feature_report(WIRELESS_REPORT_ID, WIRELESS_FEATURE_SIZE),
        )
        configuration = self.configuration or self.read_configuration()
        status: WirelessStatus = {
            **report_status,
            "wifi_enabled": bool(configuration["radios"] & 1),
            "ble_enabled": bool(configuration["radios"] & 2),
            "tcp_cli_enabled": configuration["tcp_cli_enabled"],
            "stale": False,
            "remaining_seconds": None,
        }
        if (status["pair_tested"] and status["test_generation"] == self._tested_generation
                and self._tested_at is not None):
            status["remaining_seconds"] = max(0, 600 - ceil(monotonic() - self._tested_at))
        self._wireless_active = (
            status["wifi_state_id"] == WifiState.TESTING or status["scan_state"] == "scanning"
        )
        return status

    @_serialized
    def tcp_cli_auth_proof(self, nonce: bytes, uid: bytes) -> bytes:
        """Ask this board for the fixed TCP CLI proof; never expose or copy its saved key."""
        self._require_open()
        identity = self._require(CAP_WIRELESS, "USB TCP CLI authentication is unavailable on this firmware")
        try:
            expected_uid = bytes.fromhex(identity["serial_number"])
        except (KeyError, ValueError) as error:
            raise RuntimeError("The connected board identity is unavailable") from error
        if not isinstance(uid, bytes) or len(uid) != 8 or uid != expected_uid:
            raise ProtocolError("TCP CLI challenge belongs to a different board")
        result = self._execute_control(
            build_tcp_cli_auth_request(self._next_transaction_id(), nonce), ControlOpcode.TCP_PROOF,
            retry_get=True,
        )
        self._require_completed(result)
        proof = result["proof"]
        if proof is None:
            raise ProtocolError("TCP CLI authentication result is missing its proof")
        return bytes(proof)

    @_serialized
    def set_wireless(self, settings: dict[str, object]) -> WirelessSaveResult:
        """Save board settings and credentials atomically; return wireless status plus confirmed settings."""
        self._require_open()
        if self._command_uncertain:
            raise RuntimeError("A prior command outcome is uncertain; further writes are blocked")
        validate_wireless_configuration(settings)
        previous = self.read_wireless()
        board_settings = settings["board"]
        if not isinstance(board_settings, dict):
            raise ProtocolError("Validated wireless settings are missing board configuration")
        board: dict[str, object] = {}
        for key, value in board_settings.items():
            if not isinstance(key, str):
                raise ProtocolError("Validated board setting names must be strings")
            board[key] = value
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
            for field, opcode in (("ssid", ControlOpcode.STAGE_SSID),
                                  ("password", ControlOpcode.STAGE_PASSWORD)):
                if field in settings or (field == "ssid" and "ssid_hex" in settings):
                    self._wireless_command(opcode, value=request_ssid(settings) if field == "ssid" else settings[field])
        if "ble_key" in settings:
            self._wireless_command(ControlOpcode.STAGE_KEY, value=settings["ble_key"])
        staged = self.read_wireless()
        if use_tested and (not staged["pair_tested"] or staged["test_generation"] != previous["test_generation"]):
            raise RuntimeError("The tested Wi-Fi pair changed before saving")
        commit = staged["ssid_staged"] or staged["password_staged"] or staged["key_staged"]
        expected_mask = (int(staged["ssid_staged"]) | (int(staged["password_staged"]) << 1)
                         | (int(staged["key_staged"]) << 2))
        command = build_settings_command_report(board, self._next_transaction_id(), commit=commit)
        result = self._execute_control(command, ControlOpcode.SAVE_SETTINGS)
        self._require_completed(result)
        saved = result["saved_settings"]
        radios = board["radios"]
        tcp_cli_enabled = board["tcp_cli_enabled"]
        if saved is None or type(radios) is not int or type(tcp_cli_enabled) is not bool:
            raise ProtocolError("Wireless save result is missing validated board settings")
        if saved != board or result["commit_mask"] != expected_mask:
            raise RuntimeError("Firmware settings or credential commit differ from the requested values")
        # Reset is already scheduled. Confirm saved fields from the verified commit, not a post-save GET.
        response: WirelessSaveResult = {
            **staged, "wifi_enabled": bool(radios & 1), "ble_enabled": bool(radios & 2),
            "tcp_cli_enabled": tcp_cli_enabled, "stale": True, "settings": saved,
        }
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
        command = self._wireless_command(ControlOpcode.WIFI_SCAN)
        scan = self._poll_wireless(
            "scan", previous["scan_generation"], 15.0, on_poll=on_poll,
            command_reference=(command["transaction_id"], command["opcode"]),
        )
        networks = []
        for index in range(scan["scan_count"]):
            result = self._wireless_command(ControlOpcode.WIFI_ENTRY, index=index, generation=scan["scan_generation"])
            entry = result["scan_entry"]
            if entry is None:
                raise ProtocolError("Wireless scan entry result is missing")
            if entry["generation"] != scan["scan_generation"] or entry["index"] != index:
                raise ProtocolError("Mismatched scan entry generation or index")
            networks.append(entry)
        return networks

    @_serialized
    def test_wireless(
        self, ssid: str | bytes, password: str = "", *, on_poll: Callable[[], None] | None = None,
        on_progress: Callable[[str], None] | None = None, cancel: Event | None = None,
    ) -> WirelessTestResult:
        """Test a temporary network join; firmware retains credentials only in RAM after success."""
        self._require_open()
        # Validate both fields before staging either, including opaque SSIDs selected from scans.
        build_wireless_command_report(1, ControlOpcode.STAGE_SSID, value=ssid)
        build_wireless_command_report(1, ControlOpcode.STAGE_PASSWORD, value=password)
        previous = self.read_wireless()
        self._wireless_command(ControlOpcode.STAGE_SSID, value=ssid)
        self._wireless_command(ControlOpcode.STAGE_PASSWORD, value=password)
        command = self._wireless_command(ControlOpcode.WIFI_TEST)
        status = self._poll_wireless("test", previous["test_generation"], 35.0,
                                     on_poll=on_poll, on_progress=on_progress, cancel=cancel,
                                     command_reference=(command["transaction_id"], command["opcode"]))
        self._tested_generation = status["test_generation"]
        self._tested_at = monotonic()
        return {
            "ipv4": status["ipv4"],
            "detail": status["detail"],
            "phase": status["phase"],
            "test_generation": status["test_generation"],
            "remaining_seconds": 600,
        }

    @_serialized
    def cancel_wireless_test(self) -> None:
        """Clear temporary test credentials from firmware RAM."""
        self._require_open()
        self._wireless_command(ControlOpcode.WIFI_CANCEL)
        self._uncertain_wireless_operation = None
        self._uncertain_command = None

    def _wireless_command(
        self, operation: int, *, value: object = None, index: int = 0, generation: int = 0,
    ) -> ControlResult:
        self._require(CAP_WIRELESS, "Wireless provisioning is unavailable on this firmware")
        command = build_wireless_command_report(self._next_transaction_id(), operation, value=value,
                                                index=index, generation=generation)
        result = self._execute_control(command, operation)
        self._require_completed(result, "Wireless")
        return result

    @staticmethod
    def _require_completed(result: ControlResult, action: str = "Control") -> None:
        if result["status"] != "completed":
            raise RuntimeError(
                f"{action} command {result['status']}: {result['error']} (detail {result['detail']})"
            )

    def _poll_wireless(
        self, kind: str, previous_generation: int, timeout: float, *,
        on_poll: Callable[[], None] | None = None, on_progress: Callable[[str], None] | None = None,
        cancel: Event | None = None,
        command_reference: tuple[int, int] | None = None,
    ) -> WirelessStatus:
        deadline = monotonic() + timeout
        modulus = 65535 if kind == "scan" else 255
        expected_generation = previous_generation % modulus + 1
        self._uncertain_command = command_reference
        self._uncertain_wireless_operation = (kind, expected_generation)
        while monotonic() < deadline:
            if cancel is not None and cancel.is_set():
                try:
                    self._uncertain_command = None
                    self._uncertain_wireless_operation = None
                    self.cancel_wireless_test()
                except Exception:
                    self._uncertain_command = self._uncertain_command or command_reference
                    self._uncertain_wireless_operation = (kind, expected_generation)
                    raise
                raise WirelessTestCancelled("Wi-Fi test cancelled")
            if on_poll is not None:
                on_poll()
            status = self.read_wireless()
            generation = status[f"{kind}_generation"]
            if generation not in (previous_generation, expected_generation):
                raise ValueError("Wireless operation generation changed unexpectedly; outcome is uncertain")
            if generation == expected_generation:
                failed = (
                    status["scan_state"] == "failed" if kind == "scan"
                    else status["wifi_state_id"] == WifiState.TEST_FAILED
                )
                completed = status["scan_state"] == "complete" if kind == "scan" else (
                    status["wifi_state_id"] == WifiState.TEST_JOINED and status["pair_tested"])
                if failed or completed:
                    self._uncertain_command = None
                    self._uncertain_wireless_operation = None
                    if failed:
                        raise RuntimeError(f"Wireless {kind} failed: {status['phase'] or 'Timed out'}")
                    return status
                if on_progress is not None:
                    on_progress(status["phase"] or "Connecting to Wi-Fi")
            if cancel is None:
                sleep(0.5)
            else:
                cancel.wait(0.5)
        raise TimeoutError("Wireless operation timed out; do not resend until its result is checked")

    @_serialized
    def set_msc(self, enable: bool, *, force: bool = False) -> ControlResult:
        self._require_open()
        self._require(CAP_SPI | CAP_SDIO, "MSC control is unavailable without a selected storage transport")
        return self._execute_control(
            build_msc_command_report(enable, force=force, transaction_id=self._next_transaction_id()),
            ControlOpcode.MSC,
        )

    def close(self) -> None:
        if not self.closed:
            self._device.close()
            self.closed = True
    
    @_serialized
    def set_self_test(self, action: str, size_mib: int | None = None) -> ControlResult:
        self._require_open()
        self._require(CAP_SELF_TEST, "SD self-test is unavailable on this firmware")
        command = build_self_test_command_report(action, size_mib, self._next_transaction_id())
        return self._execute_control(command, ControlOpcode.SELF_TEST)

    def _next_transaction_id(self) -> int:
        self._transaction_id = self._transaction_id % 255 + 1
        return self._transaction_id

    @_serialized
    def reconcile_outcome(
        self,
        command_reference: tuple[int, int] | None,
        wireless_reference: tuple[str, int] | None,
    ) -> tuple[ControlResult | None, WirelessStatus | None] | None:
        """Read the result slots and confirm only the exact pending command and operation."""
        self._require_open()
        if command_reference is None and wireless_reference is None:
            return None

        control_result = None
        if command_reference is not None:
            report = self._device.get_feature_report(CONTROL_REPORT_ID, CONTROL_FEATURE_SIZE)
            control_result = _parse_report(
                parse_control_result_report, report, self._supported_identity(),
            )
            if ((control_result["transaction_id"], control_result["opcode"]) != command_reference
                    or control_result["status"] in ("idle", "pending")):
                return None

        wireless_status = None
        if wireless_reference is not None:
            kind, expected_generation = wireless_reference
            wireless_status = self.read_wireless()
            if kind == "scan":
                finished = (
                    wireless_status["scan_generation"] == expected_generation
                    and wireless_status["scan_state"] in ("complete", "failed")
                )
            elif kind == "test":
                finished = (
                    wireless_status["test_generation"] == expected_generation
                    and wireless_status["wifi_state_id"] in (WifiState.TEST_JOINED, WifiState.TEST_FAILED)
                )
            else:
                return None
            if not finished:
                return None

        self._command_uncertain = False
        self._uncertain_command = None
        self._uncertain_wireless_operation = None
        if control_result is not None:
            control_result["proof"] = None
            self.last_command_result = control_result
        return control_result, wireless_status

    def _execute_control(
        self, command: bytes, opcode: int, *, reconnect: bool = False, retry_get: bool = False,
    ) -> ControlResult:
        if self.command_uncertain:
            raise RuntimeError("a prior HID command has an uncertain outcome; reconnect and inspect device state")
        self._command_uncertain = True
        transaction_id = command[2]
        self._uncertain_command = (transaction_id, opcode)
        sent = self._device.send_feature_report(list(command))
        if sent != len(command):
            raise OSError(f"failed to send HID control opcode {opcode}")
        deadline = monotonic() + 5.0
        while True:
            try:
                report = self._device.get_feature_report(CONTROL_REPORT_ID, CONTROL_FEATURE_SIZE)
                result = _parse_report(parse_control_result_report, report, self._supported_identity())
            except OSError as error:
                if retry_get and monotonic() < deadline:
                    sleep(0.05)
                    continue
                if reconnect and monotonic() < deadline:
                    self._reopen_for_result(deadline, error)
                    continue
                raise
            if result["transaction_id"] == transaction_id and result["opcode"] == opcode:
                if result["status"] != "pending":
                    self._command_uncertain = False
                    self._uncertain_command = None
                    # Proof bytes are not dashboard snapshot data.
                    public_result = result.copy()
                    public_result["proof"] = None
                    self.last_command_result = public_result
                    return result
            if monotonic() >= deadline:
                raise TimeoutError(
                    f"timed out waiting for HID control transaction {transaction_id}; outcome is uncertain"
                )
            sleep(0.05)

    def _reopen_for_result(self, deadline: float, disconnect_error: OSError) -> None:
        """Reopen only the original UID to read its outstanding result; never replay SET."""
        expected_uid = self._supported_identity()["serial_number"]
        self._device.close()
        self.closed = True
        while monotonic() < deadline:
            sleep(0.05)
            candidates = [info for info in _matching_devices()
                          if str(info.get("serial_number", "")).upper() == expected_uid]
            if not candidates:
                continue
            if len(candidates) != 1:
                raise RuntimeError("Multiple HID devices matched the original board UID")
            device = _require_hid().device()
            try:
                device.open_path(candidates[0]["path"])
                identity = _parse_report(parse_identity_report,
                                         device.get_feature_report(IDENTITY_REPORT_ID, IDENTITY_FEATURE_SIZE))
            except OSError:
                device.close()
                continue
            except ValueError:
                device.close()
                raise
            if identity["supported"] is not True or identity["serial_number"] != expected_uid:
                device.close()
                raise RuntimeError("Reconnected HID identity does not match the original board")
            self._device = device
            self.identity = identity
            self.closed = False
            return
        raise TimeoutError("Forced save result unavailable after USB detach; outcome is uncertain") from disconnect_error

    def _require_open(self) -> None:
        if self.closed:
            raise RuntimeError("PicoForge HID connection is closed")

    def __enter__(self) -> "PicoForgeHid":
        self._require_open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()