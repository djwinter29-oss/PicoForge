"""PicoForge HID report encoders and decoders."""

from __future__ import annotations

import struct

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
WIRELESS_FEATURE_SIZE = 49
WIRELESS_COMMAND_REPORT_ID = 34
WIRELESS_COMMAND_FEATURE_SIZE = 102
WIRELESS_RESULT_REPORT_ID = 35
WIRELESS_RESULT_FEATURE_SIZE = 221
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


def parse_identity_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode and validate Feature report 1."""
    payload = _feature_payload(report, IDENTITY_REPORT_ID, IDENTITY_PAYLOAD_SIZE)
    version = payload[0]
    product_id = int.from_bytes(payload[1:3], "little")
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID identity report version {version}")
    if product_id != 2:
        return {
            "version": version,
            "product_id": product_id,
            "product_name": PRODUCT_NAMES.get(product_id, f"Unknown product (0x{product_id:04X})"),
            "supported": False,
            "capabilities": 0,
            "led_available": False,
            "reboot_available": False,
            "settings_supported": False,
            "self_test_supported": False,
            "wireless_supported": False,
        }
    uid = payload[3:11]
    firmware = tuple(payload[11:14])
    mcu_id, board_model, _reserved, capabilities = payload[14:18]
    ram_total, ram_reserved = struct.unpack_from("<II", payload, 18)
    if capabilities & ~CAP_KNOWN or bool(capabilities & CAP_SPI) == bool(capabilities & CAP_SDIO):
        raise ValueError("invalid HID identity capability flags or storage transport")
    if board_model in (1, 4) and mcu_id != 1 or board_model in (2, 3) and mcu_id != 2:
        raise ValueError("board model and MCU disagree in HID identity report")
    if not ram_total or ram_reserved > ram_total:
        raise ValueError("invalid SRAM accounting in HID identity report")
    return {
        "version": version,
        "product_id": product_id,
        "product_name": PRODUCT_NAMES[product_id],
        "supported": True,
        "device_uid": uid.hex().upper(),
        "serial_number": uid.hex().upper(),
        "firmware_major": firmware[0],
        "firmware_minor": firmware[1],
        "firmware_patch": firmware[2],
        "firmware_version": ".".join(map(str, firmware)),
        "mcu_id": mcu_id,
        "mcu": _unknown_name(mcu_id, MCU_NAMES),
        "board_model_id": board_model,
        "board_model": _unknown_name(board_model, BOARD_NAMES),
        "capabilities": capabilities,
        "led_available": bool(capabilities & CAP_LED),
        "reboot_available": bool(capabilities & CAP_REBOOT),
        "settings_supported": bool(capabilities & CAP_SAVE_SETTINGS),
        "storage_transport": "spi" if capabilities & CAP_SPI else "sdio",
        "self_test_supported": bool(capabilities & CAP_SELF_TEST),
        "wireless_supported": bool(capabilities & CAP_WIRELESS),
        "ram_total_bytes": ram_total,
        "ram_reserved_bytes": ram_reserved,
    }


def build_wireless_configuration_report(settings: dict[str, object], transaction_id: int = 1) -> bytes:
    """Build Feature 31 v2: board settings, radio switches and credentials in one atomic save."""
    if not isinstance(settings, dict):
        raise ValueError("Expected wireless configuration JSON")
    if settings.get("confirm_reboot") is not True:
        raise ValueError("Explicit confirm_reboot: true is required")
    if set(settings) - {"wifi_enabled", "ble_enabled", "ssid", "password", "ble_key", "confirm_reboot", "board",
                       "use_tested_wifi"}:
        raise ValueError("Unknown wireless configuration field")
    if type(settings.get("wifi_enabled")) is not bool or type(settings.get("ble_enabled")) is not bool:
        raise ValueError("wifi_enabled and ble_enabled must be boolean")
    if "use_tested_wifi" in settings and type(settings["use_tested_wifi"]) is not bool:
        raise ValueError("use_tested_wifi must be boolean")
    use_tested = settings.get("use_tested_wifi", False)
    if "board" not in settings:
        raise ValueError("board settings are required")
    clock, profile, radios = _validate_settings(settings["board"])
    if settings["wifi_enabled"] != bool(radios & 1) or settings["ble_enabled"] != bool(radios & 2):
        raise ValueError("Wireless enable flags must match the board radio switches")
    ssid = settings.get("ssid")
    try:
        encoded = ssid.encode("utf-8") if isinstance(ssid, str) else None
    except UnicodeError:
        encoded = None
    if encoded is None or b"\0" in encoded or len(encoded) > 32 or (settings["wifi_enabled"] and not encoded):
        raise ValueError("SSID must be UTF-8, at most 32 bytes, and nonempty when Wi-Fi is enabled")
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("Wireless transaction must be 1..255")
    payload = bytearray(137)
    payload[:5] = bytes([31, 2, transaction_id, radios, 0])
    payload[134:137] = struct.pack("<HB", clock, profile)
    payload[5:5 + len(encoded)] = encoded
    if "password" not in settings and not use_tested:
        payload[4] |= 1
    elif not use_tested:
        password = settings["password"]
        if not isinstance(password, str) or password and not 8 <= len(password) <= 63 or any(
                not 32 <= ord(char) <= 126 for char in password):
            raise ValueError("Password must be empty or contain 8..63 printable ASCII characters")
        payload[38:38 + len(password)] = password.encode("ascii")
    if "ble_key" not in settings:
        payload[4] |= 2
    else:
        key = settings["ble_key"]
        if not isinstance(key, str) or len(key) != 64 or any(char not in "0123456789abcdefABCDEF" for char in key):
            raise ValueError("BLE key must contain 64 hexadecimal characters and be nonzero")
        decoded = bytes.fromhex(key)
        if not any(decoded):
            raise ValueError("BLE key must contain 64 hexadecimal characters and be nonzero")
        payload[102:134] = decoded
    if use_tested:
        payload[4] |= 4
    return bytes(payload)


def build_wireless_command_report(
    transaction_id: int, operation: int, *, page: int = 0, ssid: str = "", password: str = ""
) -> bytes:
    """Build USB-only wireless scan, paging, test-join or staged-credential clear command."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255 or operation not in (1, 2, 3, 4):
        raise ValueError("Invalid wireless command")
    payload = bytearray(WIRELESS_COMMAND_FEATURE_SIZE)
    payload[:4] = bytes([WIRELESS_COMMAND_REPORT_ID, 1, operation, transaction_id])
    if operation == 2:
        if type(page) is not int or not 0 <= page <= 2:
            raise ValueError("Invalid wireless scan page")
        payload[4] = page
    elif operation == 3:
        try:
            encoded = ssid.encode("utf-8")
        except (AttributeError, UnicodeError):
            raise ValueError("SSID must be valid UTF-8") from None
        if not encoded or len(encoded) > 32 or b"\0" in encoded:
            raise ValueError("SSID must contain 1..32 UTF-8 bytes")
        if not isinstance(password, str) or password and (
                not 8 <= len(password) <= 63 or any(not 32 <= ord(char) <= 126 for char in password)):
            raise ValueError("Password must contain 8..63 printable ASCII characters or be empty for an open network")
        secret = password.encode("ascii")
        payload[5] = len(encoded)
        payload[6] = len(secret)
        payload[7:7 + len(encoded)] = encoded
        payload[39:39 + len(secret)] = secret
    return bytes(payload)


def parse_wireless_result_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode sanitized Feature 35 scan/test results; no password is returned."""
    payload = _feature_payload(report, WIRELESS_RESULT_REPORT_ID, WIRELESS_RESULT_FEATURE_SIZE - 1)
    state, page, count, wifi_state = payload[2:6]
    if payload[0] != 1 or state > 6 or page > 2 or count > 18 or wifi_state > 3:
        raise ValueError("Invalid wireless scan/test result")
    networks = []
    for index in range(6):
        entry = payload[6 + index * 35: 41 + index * 35]
        length = entry[0]
        if length == 0:
            if any(entry):
                raise ValueError("Invalid empty wireless scan entry")
            continue
        if length > 32 or any(entry[1 + length:33]) or entry[34] > 1:
            raise ValueError("Invalid wireless scan entry")
        try:
            ssid = entry[1:1 + length].decode("utf-8")
        except UnicodeError:
            raise ValueError("Invalid wireless scan SSID") from None
        networks.append({"ssid": ssid, "rssi": int.from_bytes(entry[33:34], "little", signed=True),
                         "secured": bool(entry[34])})
    if len(networks) > count or (state != 5 and any(payload[216:220])):
        raise ValueError("Invalid wireless scan/test result fields")
    return {"transaction_id": payload[1], "state": state, "page": page, "count": count,
            "wifi_state": wifi_state, "ipv4": ".".join(map(str, payload[216:220])), "networks": networks}


def parse_wireless_status_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode sanitized Feature 30; credentials are never present in this report."""
    payload = _feature_payload(report, WIRELESS_REPORT_ID, 48)
    if (payload[0] != 1 or payload[1] & ~3 or payload[2] > 6 or payload[4] > 3
            or payload[42] > 1 or payload[43] > 1 or payload[44] > 3 or any(payload[45:])):
        raise ValueError("Invalid wireless status report")
    ssid = payload[9:42]
    nul = ssid.find(b"\0")
    if nul < 0 or any(ssid[nul:]):
        raise ValueError("Invalid wireless SSID encoding")
    try:
        name = ssid[:nul].decode("utf-8")
    except UnicodeError:
        raise ValueError("Invalid wireless SSID encoding") from None
    return {
        "wifi_enabled": bool(payload[1] & 1), "ble_enabled": bool(payload[1] & 2),
        "status": CONTROL_STATUS_NAMES[payload[2]], "transaction_id": payload[3],
        "wifi_state": ("disabled", "connecting", "connected", "failed")[payload[4]],
        "ipv4": ".".join(map(str, payload[5:9])), "ssid": name,
        "password_configured": bool(payload[42]), "ble_key_configured": bool(payload[43]),
        "error": payload[44],
    }


def parse_configuration_report(
    report: bytes | bytearray | list[int], identity: dict[str, object]
) -> dict[str, object]:
    """Decode Feature report 2 and validate transport- and board-dependent fields."""
    payload = _feature_payload(report, CONFIGURATION_REPORT_ID, CONFIGURATION_PAYLOAD_SIZE)
    version = payload[0]
    clock_profile = int.from_bytes(payload[1:3], "little")
    sd_profile = payload[3]
    uart_index, uart_tx, uart_rx = payload[4:7]
    storage_gpios = tuple(payload[7:13])
    flags, reset_cause, led_gpio, card_detect_gpio = payload[13:17]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID board configuration report version {version}")
    if clock_profile not in (0, 150, 200, 250) or sd_profile not in (0, 1):
        raise ValueError("invalid stored board configuration profile")
    if flags & ~0x1F:
        raise ValueError("unknown board configuration flags")
    radios = flags >> 3 & 3
    if radios and not identity.get("wireless_supported"):
        raise ValueError("radio enables reported without wireless capability")
    if uart_index not in (0, 1, 255):
        raise ValueError("invalid monitor UART index")
    if (uart_tx == 255) != (uart_rx == 255):
        raise ValueError("monitor UART pins must both be assigned or unavailable")
    if (uart_index == 255) != (uart_tx == 255):
        raise ValueError("monitor UART index and GPIO availability disagree")
    if flags & 0x04 and (uart_index == 255 or uart_tx == 255):
        raise ValueError("enabled monitor UART requires an index and both GPIOs")
    if bool(flags & 0x01) != (card_detect_gpio != 255):
        raise ValueError("card-detect flag and GPIO disagree")
    if bool(flags & 0x02) != (led_gpio != 255):
        raise ValueError("LED flag and GPIO disagree")
    if bool(flags & 0x02) != bool(identity["led_available"]):
        raise ValueError("identity and configuration LED availability disagree")
    if flags & 0x04 and uart_index == 255:
        raise ValueError("enabled monitor UART is unavailable")
    transport = identity["storage_transport"]
    if transport == "sdio":
        if any(gpio == 255 for gpio in storage_gpios):
            raise ValueError("SDIO configuration requires six storage GPIOs")
    elif any(gpio == 255 for gpio in storage_gpios[:4]) or any(
        gpio != 255 for gpio in storage_gpios[4:]
    ):
        raise ValueError("invalid SPI storage GPIO layout")
    board_model = identity["board_model_id"]
    mcu_id = identity["mcu_id"]
    if mcu_id in (1, 2):
        max_gpio = 29 if mcu_id == 1 or board_model in (1, 2, 3, 4) else 47
        assigned = [gpio for gpio in (uart_tx, uart_rx, led_gpio, card_detect_gpio, *storage_gpios)
                    if gpio != 255]
        if any(gpio > max_gpio for gpio in assigned):
            raise ValueError("assigned GPIO is invalid for the reported MCU and board")
    return {
        "version": version,
        "stored_sysclk_profile": clock_profile,
        "system_clock_mhz": clock_profile,
        "stored_sdcard_profile": sd_profile,
        "sdcard_profile": sd_profile,
        "radios": radios,
        "monitor_uart_index": uart_index,
        "monitor_uart_tx_gpio": uart_tx,
        "monitor_uart_rx_gpio": uart_rx,
        "storage_gpios": storage_gpios,
        "board_flags": flags,
        "reset_cause": reset_cause,
        "led_gpio": led_gpio,
        "card_detect_gpio": card_detect_gpio,
        "storage_transport": transport,
        "sdio_bus_mhz": (25 if sd_profile == 0 else 50) if transport == "sdio" else 0,
        "spi_bus_mhz": (25 if sd_profile == 0 else 50) if transport == "spi" else 0,
    }


def parse_settings_report(report: bytes | bytearray | list[int],
                          identity: dict[str, object]) -> dict[str, object]:
    """Compatibility-named decoder for the new Board Configuration report."""
    return parse_configuration_report(report, identity)


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


def build_settings_command_report(
    settings: dict[str, object], transaction_id: int = 1, *, force: bool = False
) -> bytes:
    """Build Feature report 20 opcode 6 to save persistent settings."""
    clock, profile, radios = _validate_settings(settings)
    if type(force) is not bool:
        raise ValueError("Forced settings save confirmation must be boolean")
    return build_control_command_report(transaction_id, 6, flags=int(force),
                                        arguments=struct.pack("<HBB", clock, profile, radios) + b"\0")


def build_control_command_report(
    transaction_id: int,
    opcode: int,
    *,
    flags: int = 0,
    arguments: bytes | bytearray = b"\0" * 5,
) -> bytes:
    """Build one report-ID-prefixed Feature report 20 SET request."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("transaction ID must be an integer from 1 to 255")
    if type(opcode) is not int or not 0 <= opcode <= 255:
        raise ValueError("opcode must be a byte")
    if type(flags) is not int or not 0 <= flags <= 255:
        raise ValueError("flags must be a byte")
    args = _bytes(arguments)
    if len(args) != 5:
        raise ValueError("control command arguments must contain exactly 5 bytes")
    if opcode == 1 and (flags or args[0] not in (0, 1) or any(args[1:])):
        raise ValueError("invalid LED command fields")
    if opcode == 2 and (flags != 1 or any(args)):
        raise ValueError("reboot requires confirmation and zero arguments")
    if opcode == 3 and (
        flags & ~1 or args[0] not in (0, 1, 2) or any(args[1:])
        or (args[0] == 2) != bool(flags & 1)
    ):
        raise ValueError("invalid MSC command fields or force-return confirmation")
    if opcode == 4:
        size_mib = int.from_bytes(args[1:], "little")
        if flags or args[0] not in (0, 1) or (
            args[0] == 0 and size_mib != 0
        ) or (args[0] == 1 and not 1 <= size_mib <= 1024):
            raise ValueError("invalid self-test command fields")
    if opcode == 6:
        clock, profile = struct.unpack_from("<HB", args)
        if flags & ~1 or args[3] > 3 or args[4] or clock not in (0, 150, 200, 250) or profile not in (0, 1):
            raise ValueError("invalid persistent settings command fields")
    if opcode not in (1, 2, 3, 4, 6):
        raise ValueError("control opcode is unsupported")
    return bytes((CONTROL_REPORT_ID, REPORT_VERSION, transaction_id, opcode, flags)) + args


def parse_control_result_report(
    report: bytes | bytearray | list[int], identity: dict[str, object] | None = None
) -> dict[str, object]:
    """Decode and validate the latest Feature report 20 GET result."""
    payload = _feature_payload(report, CONTROL_REPORT_ID, CONTROL_PAYLOAD_SIZE)
    version, transaction_id, opcode, status, error_reason = payload[:5]
    result_data = payload[5:9]
    data_value = int.from_bytes(result_data, "little")
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID control report version {version}")
    if status not in _ALLOWED_STATUS_ERRORS or error_reason not in _ALLOWED_STATUS_ERRORS[status]:
        raise ValueError("invalid HID control status/error combination")
    if status == CONTROL_IDLE:
        if transaction_id or opcode or any(result_data):
            raise ValueError("invalid nonempty HID control idle result")
    elif not transaction_id:
        raise ValueError("HID control result has a zero transaction ID")
    if opcode in _OPCODE_STATUSES and status not in _OPCODE_STATUSES[opcode]:
        raise ValueError("HID control status is invalid for its opcode")
    invalid_errors = {1: {1, 3}, 2: {1, 2}, 3: {1, 2, 3}, 4: {1, 3}, 6: {1, 3}}
    if status == CONTROL_INVALID and opcode in invalid_errors and error_reason not in invalid_errors[opcode]:
        raise ValueError("HID control invalid error is not valid for its opcode")
    if status == CONTROL_UNSUPPORTED and opcode in (1, 2, 3, 4, 6) and error_reason != 4:
        raise ValueError("invalid unsupported result for assigned HID opcode")
    if status == CONTROL_UNSUPPORTED and error_reason == 4 and identity is not None:
        required_capability = {1: CAP_LED, 2: CAP_REBOOT, 4: CAP_SELF_TEST, 6: CAP_SAVE_SETTINGS}.get(opcode)
        if opcode == 3 or (required_capability is not None and identity["capabilities"] & required_capability):
            raise ValueError("HID control result contradicts advertised capabilities")
    if status == CONTROL_UNSUPPORTED and opcode not in (1, 2, 3, 4, 5, 6) and error_reason != 7:
        raise ValueError("unknown HID opcode must report unsupported-opcode error")
    if opcode not in (0, 1, 2, 3, 4, 5, 6) and (status != CONTROL_UNSUPPORTED or error_reason != 7):
        raise ValueError("unknown HID opcode must be unsupported")
    if opcode == 0 and status not in (CONTROL_IDLE, CONTROL_UNSUPPORTED):
        raise ValueError("zero opcode is only valid for Idle or unsupported results")
    if opcode == 5 and status != CONTROL_UNSUPPORTED:
        raise ValueError("HID opcode 5 is unassigned")
    if opcode != 6 and any(result_data):
        raise ValueError("HID control result data is only valid for settings")
    saved_settings = None
    if opcode == 6:
        clock, profile, radios = struct.unpack("<HBB", result_data)
        if radios > 3 or (status == CONTROL_COMPLETED and (clock not in (0, 150, 200, 250) or profile not in (0, 1))):
            raise ValueError("invalid HID settings result data")
        if status != CONTROL_COMPLETED and any(result_data):
            raise ValueError("unsuccessful settings command returned result data")
        if status == CONTROL_COMPLETED:
            saved_settings = {"system_clock_mhz": clock, "sdcard_profile": profile, "radios": radios}
    return {
        "version": version,
        "transaction_id": transaction_id,
        "opcode": opcode,
        "status_id": status,
        "status": CONTROL_STATUS_NAMES[status],
        "error_reason": error_reason,
        "error": CONTROL_ERROR_NAMES.get(error_reason, f"unknown ({error_reason})"),
        "result_data": data_value,
        "saved_settings": saved_settings,
    }


def parse_info_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Compatibility-named decoder for Feature report 1."""
    return parse_identity_report(report)


def parse_status_report(
    report: bytes | bytearray | list[int], identity: dict[str, object] | None = None
) -> dict[str, object]:
    """Decode and validate Input report 3 telemetry."""
    payload = _input_payload(report, TELEMETRY_REPORT_ID, TELEMETRY_PAYLOAD_SIZE)
    version, led_flags = payload[:2]
    temperature = struct.unpack_from("<h", payload, 2)[0]
    uptime_ms = int.from_bytes(payload[4:12], "little")
    core0, core1 = struct.unpack_from("<HH", payload, 12)
    system_clock_hz = int.from_bytes(payload[16:20], "little")
    sequence, flags = payload[20:22]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID telemetry report version {version}")
    if led_flags & ~1 or flags & ~0x0F:
        raise ValueError("unknown HID telemetry flags")
    if system_clock_hz == 0:
        raise ValueError("invalid system clock in HID telemetry report")
    if any(value > 10000 and value != 0xFFFF for value in (core0, core1)):
        raise ValueError("invalid per-core CPU usage in HID telemetry report")
    temperature_valid = bool(flags & TELEMETRY_TEMPERATURE_VALID)
    led_available = bool(flags & TELEMETRY_LED_AVAILABLE)
    led_on = bool(led_flags & 1)
    if flags & TELEMETRY_CDC_OPEN and not (flags & TELEMETRY_USB_MOUNTED):
        raise ValueError("CDC DTR cannot be open when USB is unmounted")
    if not temperature_valid and temperature:
        raise ValueError("invalid temperature must be zero when the sensor is unavailable")
    if led_on and not led_available:
        raise ValueError("LED state reported without an available LED")
    if identity is not None:
        if led_available != bool(identity["led_available"]):
            raise ValueError("telemetry and identity LED availability disagree")
    return {
        "version": version,
        "sequence": sequence,
        "uptime_ms": uptime_ms,
        "system_clock_hz": system_clock_hz,
        "usb_mounted": bool(flags & TELEMETRY_USB_MOUNTED),
        "cdc_open": bool(flags & TELEMETRY_CDC_OPEN),
        "temperature_valid": temperature_valid,
        "temperature_centidegrees": temperature if temperature_valid else None,
        "led_available": led_available,
        "led_on": led_on,
        "led_flags": led_flags,
        "picoforge_flags": flags,
        "cpu_usage_core0_basis_points": None if core0 == 0xFFFF else core0,
        "cpu_usage_core1_basis_points": None if core1 == 0xFFFF else core1,
        "ram_used_bytes": identity.get("ram_reserved_bytes") if identity else None,
        "ram_total_bytes": identity.get("ram_total_bytes") if identity else None,
    }


def parse_memory_report(
    report: bytes | bytearray | list[int],
    identity: dict[str, object] | None = None,
) -> dict[str, object]:
    """Decode and validate Input report 10 storage and MSC state."""
    payload = _input_payload(report, STORAGE_REPORT_ID, STORAGE_PAYLOAD_SIZE)
    version, sequence, flags, component_type, bus_mode = payload[:5]
    clock_hz = int.from_bytes(payload[5:9], "little")
    speed_mode = payload[9]
    block_count = int.from_bytes(payload[10:14], "little")
    cid = payload[14:30]
    filesystem_id, msc_enabled, msc_pending, msc_rejected = payload[30:34]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID storage report version {version}")
    if flags & ~0x7F:
        raise ValueError("unknown HID storage flags")
    if component_type not in (0, 1) or bus_mode not in (0, 1, 2) or speed_mode not in (0, 1, 2):
        raise ValueError("unknown HID storage type, bus mode, or speed mode")
    if filesystem_id not in range(5) or any(value not in (0, 1) for value in (msc_enabled, msc_pending, msc_rejected)):
        raise ValueError("invalid HID storage filesystem or MSC state")
    configured, presence_known, present, initialized, cid_valid, capacity_valid, sample_valid = (
        bool(flags & (1 << bit)) for bit in range(7)
    )
    current_fields = (component_type, bus_mode, clock_hz, speed_mode, block_count, any(cid))
    if not sample_valid:
        if flags & 0x3E or any(current_fields):
            raise ValueError("invalid SD fields must be zero when the sample is unavailable")
    else:
        if not configured or component_type != 1:
            raise ValueError("valid SD sample requires configured storage and an SD card")
        if identity is not None:
            expected_mode = 1 if identity["storage_transport"] == "sdio" else 2
            if bus_mode != expected_mode:
                raise ValueError("storage bus mode disagrees with identity transport capability")
        if initialized:
            if not (configured and presence_known and present) or not clock_hz or speed_mode == 0:
                raise ValueError("initialized SD sample has inconsistent state or clock")
        elif speed_mode != 0 or clock_hz:
            raise ValueError("uninitialized SD sample has an active speed or clock")
        if present and not presence_known:
            raise ValueError("SD card reported present without known presence")
        if initialized and not (configured and presence_known and present):
            raise ValueError("initialized SD card requires configured, known-present storage")
        if (cid_valid or capacity_valid) and not initialized:
            raise ValueError("SD metadata validity requires an initialized card")
        if not cid_valid and any(cid):
            raise ValueError("invalid SD CID must be zero")
        if (not capacity_valid and block_count) or (capacity_valid and not block_count):
            raise ValueError("invalid SD capacity must be zero")
        if cid_valid and not any(cid):
            raise ValueError("valid SD CID cannot be all zero")
    if (msc_enabled or msc_pending) and (sample_valid or flags & 0x3E):
        raise ValueError("SD sample must be invalid during MSC ownership or transition")
    if (msc_enabled or msc_pending) and not configured:
        raise ValueError("MSC cannot own unconfigured storage")
    if not configured and flags & 0x7E:
        raise ValueError("unconfigured storage cannot have a valid SD sample")
    if initialized and not clock_hz:
        raise ValueError("initialized SD sample must report a nonzero bus clock")

    manufacturer = None
    if cid_valid:
        date = ((cid[13] & 0x0F) << 8) | cid[14]
        manufacturer = {
            "id": cid[0],
            "oem_id": cid[1:3].decode("ascii", errors="replace"),
            "product_name": cid[3:8].decode("ascii", errors="replace"),
            "product_revision": f"{cid[8] >> 4}.{cid[8] & 0x0F}",
            "serial_number": int.from_bytes(cid[9:13], "big"),
            "manufacture_year": 2000 + (date >> 4),
            "manufacture_month": date & 0x0F,
            "raw": cid.hex().upper(),
        }
    formats = ("unknown", "fat12", "fat16", "fat32", "exfat")
    return {
        "version": version,
        "sequence": sequence,
        "configured": configured,
        "sample_valid": sample_valid,
        "presence_known": presence_known if sample_valid else False,
        "present": present if sample_valid and presence_known else None,
        "initialized": initialized if sample_valid else False,
        "component_type": "sd-card" if component_type == 1 else None,
        "mode": {0: None, 1: "sdio", 2: "spi"}[bus_mode],
        "bus_clock_hz": clock_hz if sample_valid and initialized else None,
        "speed_mode": {0: None, 1: "standard", 2: "high-speed"}[speed_mode],
        "block_count": block_count if capacity_valid else None,
        "capacity_bytes": block_count * 512 if capacity_valid else None,
        "cid": cid.hex().upper() if cid_valid else None,
        "manufacturer": manufacturer,
        "filesystem_format": formats[filesystem_id],
        "msc_enabled": bool(msc_enabled),
        "msc_transition_pending": bool(msc_pending),
        "msc_request_rejected": bool(msc_rejected),
        "msc": {
            "enabled": bool(msc_enabled),
            "transition_pending": bool(msc_pending),
            "request_rejected": bool(msc_rejected),
            "force_return_supported": True,
        },
    }


def build_led_command_report(on: bool, transaction_id: int = 1) -> bytes:
    if type(on) is not bool:
        raise ValueError("LED state must be boolean")
    return build_control_command_report(transaction_id, 1, arguments=bytes((int(on), 0, 0, 0, 0)))


def build_msc_command_report(
    enable: bool,
    *,
    force: bool = False,
    transaction_id: int = 1,
) -> bytes:
    if type(enable) is not bool or type(force) is not bool or (force and enable):
        raise ValueError("Force return requires disabling MSC")
    action = 2 if force else int(enable)
    flags = 1 if force else 0
    return build_control_command_report(transaction_id, 3, flags=flags, arguments=bytes((action, 0, 0, 0, 0)))


def build_self_test_command_report(
    action: str,
    size_mib: int | None = None,
    transaction_id: int = 1,
) -> bytes:
    if action not in ("start", "cancel"):
        raise ValueError("Self-test action must be start or cancel")
    if size_mib is not None and type(size_mib) is not int:
        raise ValueError("Self-test size must be an integer")
    if action == "cancel":
        if size_mib not in (None, 0):
            raise ValueError("Self-test cancel does not accept a size")
        size_mib = 0
    elif size_mib is None:
        size_mib = 32
    elif not 1 <= size_mib <= 1024:
        raise ValueError("Self-test size must be an integer from 1 to 1024 MiB")
    args = bytes((int(action == "start"),)) + int(size_mib).to_bytes(4, "little")
    return build_control_command_report(transaction_id, 4, arguments=args)


def parse_self_test_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode and validate Input report 11 self-test progress and retained result."""
    payload = _input_payload(report, SELF_TEST_REPORT_ID, SELF_TEST_PAYLOAD_SIZE)
    version, sequence, state, flags = payload[:4]
    target, written, read, write_ms, read_ms, mismatches = struct.unpack_from("<IIIIII", payload, 4)
    error, fs_result, request_error, reserved = payload[28:32]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID self-test report version {version}")
    if flags & ~3 or reserved or target > 1024 * 1024 * 1024 or written > target or read > written:
        raise ValueError("invalid HID self-test report")
    if state in range(1, 9) and not target:
        raise ValueError("active or retained self-test state requires a target size")
    if state <= 8:
        if state == 0 and any(payload[4:29]):
            raise ValueError("idle self-test report must have zero run data")
        if state == 1 and (
            not target or any((written, read, write_ms, read_ms, mismatches, error, fs_result))
        ):
            raise ValueError("queued self-test report must have zero progress")
        if state != 7 and error:
            raise ValueError("self-test execution error is only valid in Failed state")
        if state == 7 and error <= 11 and error not in (5, 6, 8, 9, 10):
            raise ValueError("invalid execution error in Failed self-test state")
        if state == 6 and (
            not target or written != target or read != target or mismatches or error or flags & 1
        ):
            raise ValueError("invalid Passed self-test result")
    if error == 8 and state == 7:
        if not fs_result:
            raise ValueError("filesystem I/O failure requires a FatFs result")
    elif error == 10 and state == 7:
        pass
    elif fs_result:
        raise ValueError("filesystem result is only valid for applicable self-test errors")
    if request_error in (3, 9, 10):
        raise ValueError("reserved or execution-only self-test request error")
    state_names = {
        0: "idle", 1: "queued", 2: "preparing", 3: "writing",
        4: "reading", 5: "cleanup", 6: "passed", 7: "failed", 8: "cancelled",
    }
    return {
        "version": version,
        "sequence": sequence,
        "state_id": state,
        "state": _unknown_name(state, state_names),
        "active": 1 <= state <= 5,
        "target_bytes": target,
        "written_bytes": written,
        "read_bytes": read,
        "write_elapsed_ms": write_ms,
        "read_elapsed_ms": read_ms,
        "write_bytes_per_second": written * 1000 / write_ms if write_ms else None,
        "read_bytes_per_second": read * 1000 / read_ms if read_ms else None,
        "progress_percent": (written + read) * 100 / (2 * target) if target else 0,
        "mismatch_count": mismatches,
        "error": error,
        "error_name": _unknown_name(error, {
            0: "none", 1: "busy", 2: "USB owns storage", 3: "reserved",
            4: "invalid size", 5: "card unavailable", 6: "insufficient space",
            7: "test file exists", 8: "filesystem I/O", 9: "verification", 10: "cleanup",
            11: "filesystem not mounted",
        }),
        "filesystem_result": fs_result,
        "request_error": request_error,
        "request_error_name": _unknown_name(request_error, {
            0: "none", 1: "busy", 2: "USB owns storage", 4: "invalid size",
            5: "card unavailable", 6: "insufficient space", 7: "test file exists",
            8: "filesystem I/O", 11: "filesystem not mounted",
        }),
        "file_remains": bool(flags & 1),
        "volume_mounted": bool(flags & 2),
    }


parse_device_identity_report = parse_identity_report
parse_board_configuration_report = parse_configuration_report
parse_telemetry_report = parse_status_report
parse_storage_report = parse_memory_report
parse_self_test_status_report = parse_self_test_report
