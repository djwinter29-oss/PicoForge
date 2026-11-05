"""Wireless Feature 12 snapshots and Feature 20 staging codecs."""

from __future__ import annotations

from .common import REPORT_VERSION, WIRELESS_REPORT_ID, _feature_payload, _validate_settings
from .control import build_control_command_report, parse_control_result_report

WIRELESS_STATUS_SIZE = 48
WIRELESS_TEST_DETAILS = (
    "", "Radio initialization failed", "Network not found", "Password rejected",
    "Connected but no IP address from DHCP", "Timed out", "Test connection lost",
)


def ssid_bytes(ssid: object) -> bytes:
    """Normalize user text or lossless scan bytes, never a displayed escaped name."""
    try:
        if isinstance(ssid, str):
            raw = ssid.encode("utf-8")
        elif isinstance(ssid, (bytes, bytearray)):
            raw = bytes(ssid)
        elif isinstance(ssid, list) and all(type(value) is int and 0 <= value <= 255 for value in ssid):
            raw = bytes(ssid)
        else:
            raise ValueError("SSID must be text or raw bytes")
    except UnicodeError:
        raise ValueError("SSID must be valid UTF-8 text or raw bytes") from None
    if len(raw) > 32:
        raise ValueError("SSID must contain at most 32 bytes")
    return raw


def display_ssid(raw: bytes) -> str:
    """Escape invalid UTF-8 and nonprintable characters without losing wire bytes."""
    result = []
    for char in raw.decode("utf-8", errors="surrogateescape"):
        if char.isprintable():
            result.append(char)
        elif 0xDC80 <= ord(char) <= 0xDCFF:
            result.append(f"\\x{ord(char) - 0xDC00:02X}")
        else:
            result.extend(f"\\x{value:02X}" for value in char.encode("utf-8"))
    return "".join(result)


def request_ssid(settings: dict[str, object]) -> bytes:
    """Prefer the lossless scan selection over its display-only escaped label."""
    if "ssid_hex" not in settings:
        return ssid_bytes(settings["ssid"])
    encoded = settings["ssid_hex"]
    if (not isinstance(encoded, str) or len(encoded) > 64 or len(encoded) % 2
            or any(char not in "0123456789abcdefABCDEF" for char in encoded)):
        raise ValueError("ssid_hex must contain at most 32 bytes encoded as hexadecimal")
    return bytes.fromhex(encoded)


def build_wireless_command_report(transaction_id: int, operation: int, *, value: object = None,
                                  index: int = 0, generation: int = 0) -> bytes:
    """Build a USB wireless opcode (6-13) on the common control channel."""
    if operation in (6, 7, 8):
        if operation == 6:
            raw = ssid_bytes(value)
        elif operation == 7:
            if not isinstance(value, str) or (value and not 8 <= len(value) <= 63) or any(
                    not 32 <= ord(char) <= 126 for char in value):
                raise ValueError("Password must be empty or contain 8..63 printable ASCII characters")
            raw = value.encode("ascii")
        else:
            if value == "":
                raw = b""
            elif isinstance(value, str) and len(value) == 64 and all(
                    char in "0123456789abcdefABCDEF" for char in value):
                raw = bytes.fromhex(value)
            elif isinstance(value, bytes) and len(value) == 32:
                raw = value
            else:
                raise ValueError("Control key must contain 64 hexadecimal characters or be empty")
        args = bytes((len(raw),)) + raw
    elif operation == 12:
        if type(index) is not int or not 0 <= index < 18 or type(generation) is not int or not 1 <= generation <= 65535:
            raise ValueError("Invalid scan index or generation")
        args = bytes((index,)) + generation.to_bytes(2, "little")
    elif operation == 13:
        if not isinstance(value, bytes) or len(value) != 16:
            raise ValueError("TCP CLI auth nonce must contain exactly 16 bytes")
        args = value
    elif operation in (9, 10, 11):
        args = b""
    else:
        raise ValueError("Invalid wireless opcode")
    return build_control_command_report(transaction_id, operation, arguments=args)


def build_tcp_cli_auth_request(transaction_id: int, nonce: bytes) -> bytes:
    return build_wireless_command_report(transaction_id, 13, value=nonce)


def parse_tcp_cli_auth_result(report: bytes | bytearray | list[int], transaction_id: int) -> bytes:
    result = parse_control_result_report(report)
    if result["transaction_id"] != transaction_id or result["opcode"] != 13:
        raise ValueError("invalid or stale TCP CLI auth result")
    if result["status"] != "completed":
        raise RuntimeError("The board cannot create a TCP CLI proof; check the saved Control key")
    return result["proof"]


def validate_wireless_configuration(settings: object) -> None:
    """Validate all user input before the first staging write; omitted fields stay untouched."""
    if not isinstance(settings, dict):
        raise ValueError("Expected wireless configuration JSON")
    if set(settings) - {"wifi_enabled", "ble_enabled", "ssid", "ssid_hex", "password", "ble_key", "confirm_reboot",
                        "board", "use_tested_wifi", "tcp_cli_enabled"}:
        raise ValueError("Unknown wireless configuration field")
    if settings.get("confirm_reboot") is not True:
        raise ValueError("Explicit confirm_reboot: true is required")
    if type(settings.get("wifi_enabled")) is not bool or type(settings.get("ble_enabled")) is not bool:
        raise ValueError("wifi_enabled and ble_enabled must be boolean")
    if "use_tested_wifi" in settings and type(settings["use_tested_wifi"]) is not bool:
        raise ValueError("use_tested_wifi must be boolean")
    if "tcp_cli_enabled" in settings and type(settings["tcp_cli_enabled"]) is not bool:
        raise ValueError("tcp_cli_enabled must be boolean")
    if "ble_key" in settings and not isinstance(settings["ble_key"], str):
        raise ValueError("Control key must be hexadecimal text or empty")
    _, _, radios = _validate_settings(settings.get("board"))
    if settings["wifi_enabled"] != bool(radios & 1) or settings["ble_enabled"] != bool(radios & 2):
        raise ValueError("Wireless enable flags must match the board radio switches")
    if ("tcp_cli_enabled" in settings and "tcp_cli_enabled" in settings["board"]
            and settings["tcp_cli_enabled"] != settings["board"]["tcp_cli_enabled"]):
        raise ValueError("TCP CLI switch must match the board settings")
    for field, opcode in (("ssid", 6), ("password", 7), ("ble_key", 8)):
        if field in settings or (field == "ssid" and "ssid_hex" in settings):
            build_wireless_command_report(
                1, opcode, value=request_ssid(settings) if field == "ssid" else settings[field])
    if settings.get("use_tested_wifi") and "password" in settings:
        raise ValueError("A tested pair cannot be replaced while saving")


def parse_scan_entry(data: bytes) -> dict:
    generation = int.from_bytes(data[:2], "little")
    index, length = data[2:4]
    if (len(data) != 63 or not generation or index >= 18 or length > 32
            or any(data[4 + length:36]) or data[37] & ~7 or any(data[38:])):
        raise ValueError("Invalid wireless scan entry")
    raw = data[4:4 + length]
    security = data[37]
    labels = ("Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN")
    return {"generation": generation, "index": index, "ssid": display_ssid(raw),
            "ssid_hex": raw.hex(),
            "rssi": int.from_bytes(data[36:37], "little", signed=True),
            "secured": bool(security), "security": labels[security]}


def parse_wireless_status_report(report: bytes | bytearray | list[int]) -> dict:
    """Decode only runtime fields actually carried by Feature 12."""
    p = _feature_payload(report, WIRELESS_REPORT_ID, WIRELESS_STATUS_SIZE)
    flags, length, staged = p[4], p[9], p[47]
    scan, generation, count, test = p[42], int.from_bytes(p[43:45], "little"), p[45], p[46]
    if (p[0] != REPORT_VERSION or p[1] > 6 or p[3] > 2 or length > 32 or any(p[10 + length:42])
            or scan > 3 or count > 18 or staged & ~15):
        raise ValueError("Invalid wireless status report")
    # ipv4, detail and pair-tested are independent of wifi_state: a saved link may reconnect after a failed test.
    if ((scan != 2 and count) or (scan and not generation) or (p[1] >= 4 and not test)
            or (staged & 8 and (flags & 0xF0 or staged & 7))
            or any(staged & (1 << bit) and not flags & (0x10 << bit) for bit in range(3))
            or (staged & 1 and not (flags & 0x20 and staged & 2))
            or (flags & 0x80 and not flags & 0x30)
            or (flags & 4 and not flags & 2)):
        raise ValueError("Inconsistent wireless status fields")
    detail = p[2] if p[2] < len(WIRELESS_TEST_DETAILS) else 0
    raw = p[10:10 + length]
    return {
        "wifi_state": ("disabled", "connecting", "connected", "failed", "testing", "test_joined", "test_failed")[p[1]],
        "wifi_state_id": p[1], "error": detail, "detail": detail,
        "phase": WIRELESS_TEST_DETAILS[detail], "ble_state": ("off", "advertising", "connected")[p[3]],
        "network_configured": bool(flags & 1), "ble_key_configured": bool(flags & 2),
        "tcp_cli_started": bool(flags & 4), "tcp_cli_auth_required": bool(flags & 2),
        "ssid_staged": bool(flags & 16), "password_staged": bool(flags & 32),
        "key_staged": bool(flags & 64), "pair_tested": bool(flags & 128),
        "ipv4": ".".join(map(str, p[5:9])), "ssid": display_ssid(raw), "ssid_hex": raw.hex(),
        "scan_state": ("idle", "scanning", "complete", "failed")[scan],
        "scan_generation": generation, "scan_count": count, "test_generation": test, "staged_state": staged,
    }
