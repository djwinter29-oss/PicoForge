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

CONTROL_PAYLOAD_SIZE = 9

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

WIRELESS_REPORT_ID = 30

WIRELESS_FEATURE_SIZE = 54

WIRELESS_COMMAND_REPORT_ID = 34

WIRELESS_COMMAND_FEATURE_SIZE = 102

WIRELESS_RESULT_REPORT_ID = 35

WIRELESS_RESULT_FEATURE_SIZE = 223

TCP_CLI_AUTH_REPORT_ID = 36

TCP_CLI_AUTH_REQUEST_SIZE = 20

TCP_CLI_AUTH_RESULT_SIZE = 20

WIRELESS_CONFIGURATION_REPORT_ID = 31

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
    6: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED},
}

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
    """Normalize HIDAPI report-ID framing and documented zero padding."""
    data = _bytes(report)
    if len(data) >= size + 2 and data[0] == 0 and data[1] == report_id:
        offset = 2
    elif len(data) >= size + 1 and data[0] == report_id:
        offset = 1
    else:
        raise ValueError(f"unexpected HID Feature report {report_id} length or ID")
    payload = data[offset:offset + size]
    if len(payload) != size or any(data[offset + size:]):
        raise ValueError(f"unexpected HID Feature report {report_id} length or padding")
    return payload

def _input_payload(report: bytes | bytearray | list[int], report_id: int, size: int) -> bytes:
    data = _bytes(report)
    if len(data) != size + 1 or data[0] != report_id:
        raise ValueError(f"HID Input report {report_id} must have exactly {size} payload bytes and the correct ID")
    return data[1:]

def _unknown_name(value: int, names: dict[int, str]) -> str:
    return names.get(value, f"unknown ({value})")

def _validate_settings(settings: dict[str, object]) -> tuple[int, int, int]:
    """Return clock, SD profile and radio enables (bit 0 Wi-Fi, bit 1 BLE; 0 when omitted)."""
    if not isinstance(settings, dict):
        raise ValueError("boot settings must be an object")
    clock = settings.get("system_clock_mhz")
    profile = settings.get("sdcard_profile")
    radios = settings.get("radios", 0)
    if (type(clock) is not int or clock not in (0, 150, 200, 250) or type(profile) is not int or profile not in (0, 1)
            or type(radios) is not int or radios not in range(4)):
        raise ValueError("unsupported boot settings value")
    return clock, profile, radios

def normalize_legacy_settings(
    settings: dict[str, object], transport: str, current_profile: int | None = None
) -> dict[str, int]:
    """Translate the established dashboard settings shape to the v1 profile."""
    if not isinstance(settings, dict):
        raise ValueError("boot settings must be an object")
    clock = settings.get("system_clock_mhz")
    sdio = settings.get("sdio_bus_mhz")
    spi = settings.get("spi_bus_mhz")
    if (type(clock) is not int or clock not in (0, 150, 200, 250)
            or type(sdio) is not int or sdio not in (0, 25, 50)
            or type(spi) is not int or spi not in (0, 25, 50)):
        raise ValueError("unsupported boot settings value")
    if transport not in ("sdio", "spi"):
        raise ValueError("unknown storage transport")
    selected = sdio if transport == "sdio" else spi
    other = spi if transport == "sdio" else sdio
    if other not in (0, selected):
        raise ValueError("settings for the unselected storage transport must be zero or match the selected profile")
    profile = int(selected == 50)
    if selected == 0 and other == 0 and current_profile in (0, 1):
        profile = current_profile
    return {"system_clock_mhz": clock, "sdcard_profile": profile}

def validate_legacy_settings(settings: dict[str, object]) -> None:
    if not isinstance(settings, dict):
        raise ValueError("boot settings must be an object")
    values = (
        settings.get("system_clock_mhz"),
        settings.get("sdio_bus_mhz"),
        settings.get("spi_bus_mhz"),
    )
    if (type(values[0]) is not int or values[0] not in (0, 150, 200, 250)
            or type(values[1]) is not int or values[1] not in (0, 25, 50)
            or type(values[2]) is not int or values[2] not in (0, 25, 50)):
        raise ValueError("unsupported boot settings value")
