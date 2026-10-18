"""HIDAPI transport for PicoForge's vendor-defined status interface."""

from __future__ import annotations

from typing import Any

from .protocol import (
    INFO_PAYLOAD_SIZE,
    INFO_REPORT_ID,
    MEMORY_INPUT_SIZE,
    MEMORY_REPORT_ID,
    FILESYSTEM_REPORT_ID,
    MSC_COMMAND_REPORT_ID,
    SELF_TEST_REPORT_ID,
    SELF_TEST_INPUT_SIZE,
    STATUS_INPUT_SIZE,
    build_led_command_report,
    build_msc_command_report,
    build_self_test_command_report,
    parse_info_report,
    parse_memory_report,
    parse_filesystem_report,
    parse_msc_status_report,
    parse_self_test_report,
    parse_status_report,
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


def _select_device(serial_number: str | None, device_path: str | None) -> dict[str, Any]:
    if serial_number is not None and device_path is not None:
        raise ValueError("--serial and --device-path cannot be used together")
    devices = _require_hid().enumerate(VENDOR_ID, 0)
    matches = [info for info in devices
               if info.get("product_id") in PRODUCT_IDS and _matches_vendor_collection(info)]
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

    def read_info(self) -> dict[str, object]:
        self._require_open()
        report = self._device.get_feature_report(INFO_REPORT_ID, INFO_PAYLOAD_SIZE + 1)
        return parse_info_report(report)

    def read_status(self, timeout_ms: int = 250) -> dict[str, object] | None:
        if timeout_ms < 0:
            raise ValueError("timeout_ms must not be negative")
        self._require_open()
        report = self._device.read(STATUS_INPUT_SIZE, timeout_ms)
        return parse_status_report(report) if report else None

    def read_input_report(self, timeout_ms: int = 250) -> dict[str, object] | None:
        if timeout_ms < 0:
            raise ValueError("timeout_ms must not be negative")
        self._require_open()
        report = self._device.read(max(STATUS_INPUT_SIZE, MEMORY_INPUT_SIZE, SELF_TEST_INPUT_SIZE), timeout_ms)
        if not report:
            return None
        if report[0] == MEMORY_REPORT_ID:
            return {"kind": "memory", "data": parse_memory_report(report)}
        if report[0] == FILESYSTEM_REPORT_ID:
            return {"kind": "filesystem", "data": parse_filesystem_report(report)}
        if report[0] == MSC_COMMAND_REPORT_ID:
            return {"kind": "msc", "data": parse_msc_status_report(report)}
        if report[0] == SELF_TEST_REPORT_ID:
            return {"kind": "self_test", "data": parse_self_test_report(report)}
        return {"kind": "runtime", "data": parse_status_report(report)}

    def set_led(self, on: bool) -> None:
        self._require_open()
        report = build_led_command_report(on)
        result = self._device.send_feature_report(list(report))
        if result < 0:
            raise OSError("failed to send HID LED command")

    def set_msc(self, enable: bool, *, force: bool = False) -> None:
        self._require_open()
        report = build_msc_command_report(enable, force=force)
        result = self._device.send_feature_report(list(report))
        if result < 0:
            raise OSError("failed to send HID MSC command")

    def close(self) -> None:
        if not self.closed:
            self._device.close()
            self.closed = True

    def set_self_test(self, action: str, size_mib: int = 32) -> None:
        self._require_open()
        result = self._device.send_feature_report(list(build_self_test_command_report(action, size_mib)))
        if result < 0:
            raise OSError("failed to send HID self-test command")

    def _require_open(self) -> None:
        if self.closed:
            raise RuntimeError("PicoForge HID connection is closed")

    def __enter__(self) -> "PicoForgeHid":
        self._require_open()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()