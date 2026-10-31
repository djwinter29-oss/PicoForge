"""HIDAPI transport for PicoForge's vendor-defined status interface."""

from __future__ import annotations

from math import ceil
from time import monotonic, sleep
from typing import Any, Callable

from .protocol import (
    CAP_WIRELESS,
    WIRELESS_REPORT_ID,
    WIRELESS_FEATURE_SIZE,
    WIRELESS_RESULT_REPORT_ID,
    WIRELESS_RESULT_FEATURE_SIZE,
    build_wireless_configuration_report,
    build_wireless_command_report,
    parse_wireless_status_report,
    parse_wireless_result_report,
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
    parse_device_identity_report,
    parse_storage_report,
    parse_board_configuration_report,
    parse_control_result_report,
    parse_self_test_status_report,
    parse_telemetry_report,
)

VENDOR_ID = 0xCAFE
PRODUCT_ID = 0x4011
PRODUCT_IDS = {0x4011, 0x4012}
USAGE_PAGE = 0xFF00
USAGE = 0x0001
PRODUCT_STRING = "PicoForge CDC + HID"

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
        return product_string in (PRODUCT_STRING, "PicoForge CDC + HID + MSC")
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
        raise RuntimeError("PicoForge HID interface not found; check the USB connection and HID permissions")
    if len(matches) != 1:
        raise RuntimeError("multiple PicoForge HID interfaces matched; select one with --serial or --device-path")
    return matches[0]


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
        self.identity: dict[str, object] | None = None
        self.configuration: dict[str, object] | None = None
        self._transaction_id = 0
        self.last_command_result: dict[str, object] | None = None
        self._command_uncertain = False

    def read_info(self) -> dict[str, object]:
        self._require_open()
        report = self._device.get_feature_report(IDENTITY_REPORT_ID, IDENTITY_FEATURE_SIZE)
        self.identity = parse_device_identity_report(report)
        if (self._expected_serial_number and self.identity.get("supported")
                and str(self._expected_serial_number).upper() != self.identity["serial_number"]):
            raise ValueError("HID identity UID disagrees with the USB serial string")
        return self.identity

    def read_configuration(self) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity.get("supported"):
            raise RuntimeError(f"unsupported product: {identity['product_name']}")
        report = self._device.get_feature_report(CONFIGURATION_REPORT_ID, CONFIGURATION_FEATURE_SIZE)
        self.configuration = parse_board_configuration_report(report, identity)
        return self.configuration

    def read_settings(self) -> dict[str, int]:
        configuration = self.read_configuration()
        return {
            "system_clock_mhz": configuration["system_clock_mhz"],
            "sdcard_profile": configuration["sdcard_profile"],
            "radios": configuration["radios"],
        }

    def read_status(self, timeout_ms: int = 250) -> dict[str, object] | None:
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

    def read_input_report(self, timeout_ms: int = 250) -> dict[str, object] | None:
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

    def set_led(self, on: bool) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_LED:
            raise RuntimeError("board identification LED is unavailable")
        return self._execute_control(build_led_command_report(on, self._next_transaction_id()), 1)

    def set_settings(self, settings: dict[str, object], *, force: bool = False) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_SAVE_SETTINGS:
            raise RuntimeError("persistent settings are unavailable on this firmware")
        if force and not identity["capabilities"] & (1 << 6):
            raise RuntimeError("Update firmware for confirmed settings save while USB owns storage")
        command = build_settings_command_report(settings, self._next_transaction_id(), force=force)
        result = self._execute_control(command, 6, reconnect=force)
        if result["status"] != "completed":
            raise RuntimeError(f"settings command {result['status']}: {result['error']}")
        saved = result["saved_settings"]
        if saved != {**settings, "radios": settings.get("radios", 0)}:
            self._command_uncertain = True
            raise RuntimeError("firmware did not confirm the selected boot settings")
        return saved

    def reboot(self) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_REBOOT:
            raise RuntimeError("standalone reboot is unavailable on this firmware")
        command = build_control_command_report(self._next_transaction_id(), 2, flags=1)
        return self._execute_control(command, 2)

    def read_wireless(self) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity.get("capabilities", 0) & CAP_WIRELESS:
            raise RuntimeError("Wireless provisioning is unavailable on this firmware")
        return parse_wireless_status_report(
            self._device.get_feature_report(WIRELESS_REPORT_ID, WIRELESS_FEATURE_SIZE))

    def set_wireless(self, settings: dict[str, object]) -> dict[str, object]:
        """Save board settings and credentials atomically; return wireless status plus confirmed settings."""
        self._require_open()
        command = build_wireless_configuration_report(settings, self._next_transaction_id())
        if self._command_uncertain:
            raise RuntimeError("A prior command outcome is uncertain; further writes are blocked")
        previous = self.read_wireless()
        if previous["status"] in ("pending", "busy"):
            raise RuntimeError("Wireless provisioning is busy")
        self._command_uncertain = True
        try:
            if self._device.send_feature_report(list(command)) != len(command):
                raise OSError("Wireless provisioning delivery failed; outcome is uncertain")
            deadline = monotonic() + 5.0
            while True:
                result = self.read_wireless()
                if result["transaction_id"] == command[2] and result["status"] != "pending":
                    if result["status"] == "completed":
                        expected = {
                            "wifi_enabled": settings["wifi_enabled"],
                            "ble_enabled": settings["ble_enabled"],
                            "ssid": command[5:38].split(b"\0", 1)[0].decode("utf-8"),
                            "password_configured": (True if settings.get("use_tested_wifi")
                                else previous["password_configured"] if command[4] & 1 else True),
                            "ble_key_configured": previous["ble_key_configured"] if command[4] & 2 else True,
                        }
                        if any(result[key] != value for key, value in expected.items()):
                            raise RuntimeError("Wireless saved configuration was not confirmed; outcome is uncertain")
                        board = settings["board"]
                        saved = self.read_settings()
                        if saved != {**board, "radios": board.get("radios", 0)}:
                            raise RuntimeError("Saved board settings were not confirmed; outcome is uncertain")
                        self._command_uncertain = False
                        return {**result, "settings": saved}
                    if result["status"] in ("invalid", "unsupported", "busy", "failed"):
                        self._command_uncertain = False
                        raise RuntimeError(f"Wireless provisioning {result['status']}")
                if monotonic() >= deadline:
                    raise TimeoutError("Wireless provisioning timed out; outcome is uncertain; command was not resent")
                sleep(0.05)
        except OSError:
            raise OSError("Wireless provisioning delivery failed; outcome is uncertain; command was not resent") from None

    def scan_wireless(self, *, on_poll: Callable[[], None] | None = None) -> list[dict[str, object]]:
        """Scan for nearby access points and return strongest unique SSIDs."""
        self._require_open()
        scan = self._wireless_command(1, timeout=15.0, on_poll=on_poll)
        networks: list[dict[str, object]] = []
        for page in range((int(scan["count"]) + 5) // 6):
            networks.extend(self._wireless_command(2, page=page, on_poll=on_poll)["networks"])
        return networks

    def test_wireless(
        self, ssid: str, password: str = "", *, on_poll: Callable[[], None] | None = None,
        on_progress: Callable[[str], None] | None = None,
    ) -> dict[str, object]:
        """Test a temporary network join; firmware retains credentials only in RAM after success."""
        self._require_open()
        return self._wireless_command(3, ssid=ssid, password=password, timeout=35.0, terminal=5, on_poll=on_poll,
                                      on_progress=on_progress)

    def cancel_wireless_test(self) -> None:
        """Clear temporary test credentials from firmware RAM."""
        self._require_open()
        self._wireless_command(4)

    def _wireless_command(
        self, operation: int, *, page: int = 0, ssid: str = "", password: str = "",
        timeout: float = 15.0, terminal: int | None = None,
        on_poll: Callable[[], None] | None = None, on_progress: Callable[[str], None] | None = None,
    ) -> dict[str, object]:
        if self._command_uncertain:
            raise RuntimeError("A prior command outcome is uncertain; further writes are blocked")
        previous = self.read_wireless()
        if previous["status"] in ("pending", "busy"):
            raise RuntimeError("Wireless provisioning is busy")
        transaction = self._next_transaction_id()
        command = build_wireless_command_report(transaction, operation, page=page, ssid=ssid, password=password)
        self._command_uncertain = True
        try:
            if self._device.send_feature_report(list(command)) != len(command):
                raise OSError("Wireless command delivery failed; outcome is uncertain")
        except OSError:
            raise OSError("Wireless command delivery failed; outcome is uncertain; command was not resent") from None
        deadline = monotonic() + timeout
        expected = terminal if terminal is not None else {1: 2, 2: 2, 3: 5, 4: 0}[operation]
        while monotonic() < deadline:
            if on_poll is not None:
                on_poll()
            result = parse_wireless_result_report(
                self._device.get_feature_report(WIRELESS_RESULT_REPORT_ID, WIRELESS_RESULT_FEATURE_SIZE))
            if result["transaction_id"] == transaction:
                if result["state"] == 6:
                    self._command_uncertain = False
                    if operation == 3:
                        raise RuntimeError(f"Could not connect: {result['phase'] or 'Timed out'}")
                    raise RuntimeError("Wireless operation failed")
                if result["state"] == expected:
                    self._command_uncertain = False
                    return result
                if on_progress is not None and result["state"] == 4:
                    on_progress(str(result["phase"]))
            sleep(0.1)
        raise TimeoutError("Wireless operation timed out; do not resend until its result is checked")

    def set_msc(self, enable: bool, *, force: bool = False) -> dict[str, object]:
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

    def set_self_test(self, action: str, size_mib: int | None = None) -> dict[str, object]:
        self._require_open()
        identity = self.identity or self.read_info()
        if not identity["capabilities"] & CAP_SELF_TEST:
            raise RuntimeError("SD self-test is unavailable on this firmware")
        command = build_self_test_command_report(action, size_mib, self._next_transaction_id())
        return self._execute_control(command, 4)

    def _next_transaction_id(self) -> int:
        self._transaction_id = self._transaction_id % 255 + 1
        return self._transaction_id

    def _execute_control(self, command: bytes, opcode: int, *, reconnect: bool = False) -> dict[str, object]:
        if self._command_uncertain:
            raise RuntimeError("a prior HID command has an uncertain outcome; reconnect and inspect device state")
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
                    self.last_command_result = result
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
                identity = parse_device_identity_report(
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