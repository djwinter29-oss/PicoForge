"""Shared HID report constants and input validation helpers."""

from __future__ import annotations

IDENTITY_REPORT_ID = 1

CONFIGURATION_REPORT_ID = 2

TELEMETRY_REPORT_ID = 3

STORAGE_REPORT_ID = 10

SELF_TEST_REPORT_ID = 11

CONTROL_REPORT_ID = 20

BOARD_CONFIGURATION_REPORT_ID = CONFIGURATION_REPORT_ID

BOARD_TELEMETRY_REPORT_ID = TELEMETRY_REPORT_ID

STORAGE_STATUS_REPORT_ID = STORAGE_REPORT_ID

CONTROL_COMMAND_REPORT_ID = CONTROL_REPORT_ID

IDENTITY_PAYLOAD_SIZE = 26

CONFIGURATION_PAYLOAD_SIZE = 17

TELEMETRY_PAYLOAD_SIZE = 22

STORAGE_PAYLOAD_SIZE = 34

SELF_TEST_PAYLOAD_SIZE = 32

CONTROL_PAYLOAD_SIZE = 68

IDENTITY_FEATURE_SIZE = IDENTITY_PAYLOAD_SIZE + 1

CONFIGURATION_FEATURE_SIZE = CONFIGURATION_PAYLOAD_SIZE + 1

TELEMETRY_INPUT_SIZE = TELEMETRY_PAYLOAD_SIZE + 1

STORAGE_INPUT_SIZE = STORAGE_PAYLOAD_SIZE + 1

SELF_TEST_INPUT_SIZE = SELF_TEST_PAYLOAD_SIZE + 1

CONTROL_FEATURE_SIZE = CONTROL_PAYLOAD_SIZE + 1

REPORT_VERSION = 1

MCU_NAMES = {1: "RP2040", 2: "RP2350"}

BOARD_NAMES = {0: "Unknown / unspecified", 1: "Pico", 2: "Pico 2", 3: "Pico 2 W", 4: "Pico W"}

PRODUCT_NAMES = {0: "Unspecified", 1: "PicoUart", 2: "PicoForge"}

CAP_LED = 1 << 0

CAP_REBOOT = 1 << 1

CAP_SAVE_SETTINGS = 1 << 2

CAP_SPI = 1 << 3

CAP_SDIO = 1 << 4

CAP_SELF_TEST = 1 << 5

CAP_FORCE_SAVE = 1 << 6

CAP_WIRELESS = 1 << 7

CAP_KNOWN = 255

WIRELESS_REPORT_ID = 12

WIRELESS_FEATURE_SIZE = 49

TELEMETRY_USB_MOUNTED = 1 << 0

TELEMETRY_CDC_OPEN = 1 << 1

TELEMETRY_TEMPERATURE_VALID = 1 << 2

TELEMETRY_LED_AVAILABLE = 1 << 3

CONTROL_IDLE = 0

CONTROL_PENDING = 1

CONTROL_COMPLETED = 2

CONTROL_INVALID = 3

CONTROL_UNSUPPORTED = 4

CONTROL_BUSY = 5

CONTROL_FAILED = 6

CONTROL_STATUS_NAMES = {
    0: "idle", 1: "pending", 2: "completed", 3: "invalid",
    4: "unsupported", 5: "busy", 6: "failed",
}

CONTROL_ERROR_NAMES = {
    0: "none", 1: "malformed request", 2: "confirmation required",
    3: "invalid argument", 4: "capability unavailable", 5: "busy",
    6: "operation failed", 7: "unsupported opcode",
}

_ALLOWED_STATUS_ERRORS = {
    CONTROL_IDLE: {0},
    CONTROL_PENDING: {0},
    CONTROL_COMPLETED: {0},
    CONTROL_INVALID: {1, 2, 3},
    CONTROL_UNSUPPORTED: {4, 7},
    CONTROL_BUSY: {5},
    CONTROL_FAILED: {6},
}

_OPCODE_STATUSES = {
    1: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_FAILED},
    2: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED},
    3: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_BUSY, CONTROL_FAILED},
    4: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED},
    5: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED},
}
for _opcode in range(6, 14):
    _OPCODE_STATUSES[_opcode] = {CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_FAILED}
    if _opcode not in (10, 13):
        _OPCODE_STATUSES[_opcode].add(CONTROL_BUSY)
    if _opcode in (9, 11):
        _OPCODE_STATUSES[_opcode].add(CONTROL_PENDING)

def _bytes(report: bytes | bytearray | list[int]) -> bytes:
    if not isinstance(report, (bytes, bytearray, list)):
        raise ValueError("HID report must be bytes or a list of byte values")
    if isinstance(report, list) and any(
        type(value) is not int or not 0 <= value <= 255 for value in report
    ):
        raise ValueError("HID report list must contain byte values")
    try:
        return bytes(report)
    except (TypeError, ValueError) as error:
        raise ValueError("HID report must contain byte values") from error

def _feature_payload(report: bytes | bytearray | list[int], report_id: int, size: int) -> bytes:
    """Validate HIDAPI's report-ID-prefixed buffer without guessing framing bytes."""
    data = _bytes(report)
    if len(data) != size + 1 or data[0] != report_id:
        raise ValueError(f"unexpected HID Feature report {report_id} length or ID")
    return data[1:]

def _input_payload(report: bytes | bytearray | list[int], report_id: int, size: int) -> bytes:
    data = _bytes(report)
    if len(data) != size + 1 or data[0] != report_id:
        raise ValueError(f"HID Input report {report_id} must have exactly {size} payload bytes and the correct ID")
    return data[1:]

def _unknown_name(value: int, names: dict[int, str]) -> str:
    return names.get(value, f"unknown ({value})")

class InvalidRequest(ValueError):
    """User-supplied command input was rejected before any HID write."""


def _validate_settings(settings: dict[str, object]) -> tuple[int, int, int]:
    """Return clock, SD profile and radio enables (bit 0 Wi-Fi, bit 1 BLE)."""
    if not isinstance(settings, dict):
        raise InvalidRequest("boot settings must be an object")
    clock = settings.get("system_clock_mhz")
    profile = settings.get("sdcard_profile")
    radios = settings.get("radios")
    if (type(clock) is not int or clock not in (0, 150, 200, 250) or type(profile) is not int or profile not in (0, 1)
            or type(radios) is not int or radios not in range(4)):
        raise InvalidRequest("unsupported boot settings value")
    if "tcp_cli_enabled" in settings and type(settings["tcp_cli_enabled"]) is not bool:
        raise InvalidRequest("TCP CLI setting must be boolean")
    if set(settings) - {"system_clock_mhz", "sdcard_profile", "radios", "tcp_cli_enabled"}:
        raise InvalidRequest("unknown boot settings field")
    return clock, profile, radios
