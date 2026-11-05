"""Shared HID report constants and input validation helpers."""

from __future__ import annotations

from enum import IntEnum, IntFlag


class ControlOpcode(IntEnum):
    """Feature 20 command opcodes defined by the firmware contract."""

    LED = 1
    REBOOT = 2
    MSC = 3
    SELF_TEST = 4
    SAVE_SETTINGS = 5
    STAGE_SSID = 6
    STAGE_PASSWORD = 7
    STAGE_KEY = 8
    WIFI_TEST = 9
    WIFI_CANCEL = 10
    WIFI_SCAN = 11
    WIFI_ENTRY = 12
    TCP_PROOF = 13


class ControlStatus(IntEnum):
    """Feature 20 result states."""

    IDLE = 0
    PENDING = 1
    COMPLETED = 2
    INVALID = 3
    UNSUPPORTED = 4
    BUSY = 5
    FAILED = 6


class ControlError(IntEnum):
    """Feature 20 result error reasons."""

    NONE = 0
    MALFORMED = 1
    CONFIRMATION = 2
    ARGUMENT = 3
    CAPABILITY = 4
    BUSY = 5
    OPERATION = 6
    OPCODE = 7


class ControlDetail(IntEnum):
    """Assigned Feature 20 result detail codes."""

    NONE = 0
    CONFLICTING_WORK = 1
    USB_MSC_OWNS_STORAGE = 2
    INVALID_BENCHMARK_SIZE = 3
    CARD_UNAVAILABLE = 4
    INSUFFICIENT_SPACE = 5
    BENCHMARK_FILE_EXISTS = 6
    PREFLIGHT_FILESYSTEM_IO = 7
    BENCHMARK_VERIFICATION = 8
    BENCHMARK_CLEANUP = 9
    FILESYSTEM_NOT_MOUNTED = 10
    SSID_LENGTH_INVALID = 16
    PASSWORD_INVALID = 17
    CONTROL_KEY_LENGTH_INVALID = 18
    CREDENTIAL_PAIR_INCOMPLETE = 19
    STAGED_VALUES_EXPIRED = 20
    NO_ELIGIBLE_STAGED_VALUES = 21
    SCAN_GENERATION_STALE = 22
    SCAN_INDEX_OUT_OF_RANGE = 23
    CONTROL_KEY_NOT_SAVED = 24
    WIFI_TEST_ACTIVE = 25
    WIFI_SCAN_ACTIVE = 26
    STAGED_CREDENTIAL_COMMIT_REQUIRED = 27
    USB_STAGED_CREDENTIALS_BLOCK_BLE_SAVE = 28


class WifiState(IntEnum):
    """Feature 12 Wi-Fi state values."""

    DISABLED = 0
    CONNECTING = 1
    CONNECTED = 2
    FAILED = 3
    TESTING = 4
    TEST_JOINED = 5
    TEST_FAILED = 6


class BleState(IntEnum):
    """Feature 12 Bluetooth state values."""

    OFF = 0
    ADVERTISING = 1
    CONNECTED = 2


class WirelessFlags(IntFlag):
    """Feature 12 status bits."""

    NETWORK_CONFIGURED = 1 << 0
    BLE_KEY_CONFIGURED = 1 << 1
    TCP_CLI_STARTED = 1 << 2
    SSID_STAGED = 1 << 4
    PASSWORD_STAGED = 1 << 5
    KEY_STAGED = 1 << 6
    PAIR_TESTED = 1 << 7


class WirelessStagedFlags(IntFlag):
    """Feature 12 credential-staging bits."""

    SSID = 1 << 0
    PASSWORD = 1 << 1
    KEY = 1 << 2
    TESTED_PAIR = 1 << 3


class WifiTestDetail(IntEnum):
    """Feature 12 Wi-Fi detail values."""

    NONE = 0
    RADIO_INITIALIZATION_FAILED = 1
    NETWORK_NOT_FOUND = 2
    PASSWORD_REJECTED = 3
    NO_DHCP_ADDRESS = 4
    TIMED_OUT = 5
    TESTED_LINK_LOST = 6


class ProtocolError(ValueError):
    """A device report or protocol response violates the wire contract."""


IDENTITY_REPORT_ID = 1

CONFIGURATION_REPORT_ID = 2

TELEMETRY_REPORT_ID = 3

STORAGE_REPORT_ID = 10

SELF_TEST_REPORT_ID = 11

CONTROL_REPORT_ID = 20

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

CONTROL_IDLE = ControlStatus.IDLE
CONTROL_PENDING = ControlStatus.PENDING
CONTROL_COMPLETED = ControlStatus.COMPLETED
CONTROL_INVALID = ControlStatus.INVALID
CONTROL_UNSUPPORTED = ControlStatus.UNSUPPORTED
CONTROL_BUSY = ControlStatus.BUSY
CONTROL_FAILED = ControlStatus.FAILED

CONTROL_STATUS_NAMES = {
    ControlStatus.IDLE: "idle", ControlStatus.PENDING: "pending",
    ControlStatus.COMPLETED: "completed", ControlStatus.INVALID: "invalid",
    ControlStatus.UNSUPPORTED: "unsupported", ControlStatus.BUSY: "busy",
    ControlStatus.FAILED: "failed",
}

CONTROL_ERROR_NAMES = {
    ControlError.NONE: "none", ControlError.MALFORMED: "malformed request",
    ControlError.CONFIRMATION: "confirmation required", ControlError.ARGUMENT: "invalid argument",
    ControlError.CAPABILITY: "capability unavailable", ControlError.BUSY: "busy",
    ControlError.OPERATION: "operation failed", ControlError.OPCODE: "unsupported opcode",
}

_ALLOWED_STATUS_ERRORS = {
    CONTROL_IDLE: {ControlError.NONE},
    CONTROL_PENDING: {ControlError.NONE},
    CONTROL_COMPLETED: {ControlError.NONE},
    CONTROL_INVALID: {ControlError.MALFORMED, ControlError.CONFIRMATION, ControlError.ARGUMENT},
    CONTROL_UNSUPPORTED: {ControlError.CAPABILITY, ControlError.OPCODE},
    CONTROL_BUSY: {ControlError.BUSY},
    CONTROL_FAILED: {ControlError.OPERATION},
}

_OPCODE_STATUSES = {
    ControlOpcode.LED: {
        CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_FAILED,
    },
    ControlOpcode.REBOOT: {
        CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED,
    },
    ControlOpcode.MSC: {CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_BUSY, CONTROL_FAILED},
    ControlOpcode.SELF_TEST: {
        CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED,
    },
    ControlOpcode.SAVE_SETTINGS: {
        CONTROL_PENDING, CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_BUSY, CONTROL_FAILED,
    },
}
for _opcode in tuple(ControlOpcode)[5:]:
    _OPCODE_STATUSES[_opcode] = {CONTROL_COMPLETED, CONTROL_INVALID, CONTROL_UNSUPPORTED, CONTROL_FAILED}
    if _opcode not in (ControlOpcode.WIFI_CANCEL, ControlOpcode.TCP_PROOF):
        _OPCODE_STATUSES[_opcode].add(CONTROL_BUSY)
    if _opcode in (ControlOpcode.WIFI_TEST, ControlOpcode.WIFI_SCAN):
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


def _validate_settings(settings: object) -> tuple[int, int, int]:
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
