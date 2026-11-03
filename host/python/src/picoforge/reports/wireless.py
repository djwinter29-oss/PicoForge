"""Wireless HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import WirelessOperation, WirelessStatus

from .common import (
    CONTROL_STATUS_NAMES,
    WIRELESS_COMMAND_FEATURE_SIZE,
    WIRELESS_COMMAND_REPORT_ID,
    WIRELESS_REPORT_ID,
    WIRELESS_RESULT_FEATURE_SIZE,
    WIRELESS_RESULT_REPORT_ID,
)
from .common import _bytes, _feature_payload, _validate_settings


def build_wireless_configuration_report(
    settings: dict[str, object], transaction_id: int = 1, *, tcp_cli_configurable: bool = True,
    tcp_cli_auth_configurable: bool = True,
) -> bytes:
    """Build Feature 31 v2: board settings, radio switches and credentials in one atomic save."""
    if not isinstance(settings, dict):
        raise ValueError("Expected wireless configuration JSON")
    if settings.get("confirm_reboot") is not True:
        raise ValueError("Explicit confirm_reboot: true is required")
    if set(settings) - {"wifi_enabled", "ble_enabled", "ssid", "password", "ble_key", "confirm_reboot", "board",
                       "use_tested_wifi", "tcp_cli_enabled", "tcp_cli_auth_required"}:
        raise ValueError("Unknown wireless configuration field")
    if type(settings.get("wifi_enabled")) is not bool or type(settings.get("ble_enabled")) is not bool:
        raise ValueError("wifi_enabled and ble_enabled must be boolean")
    if "use_tested_wifi" in settings and type(settings["use_tested_wifi"]) is not bool:
        raise ValueError("use_tested_wifi must be boolean")
    use_tested = settings.get("use_tested_wifi", False)
    if "tcp_cli_enabled" in settings and (
            type(settings["tcp_cli_enabled"]) is not bool or not tcp_cli_configurable):
        raise ValueError("TCP CLI setting requires a boolean and wireless status v2")
    if "tcp_cli_auth_required" in settings and (
            type(settings["tcp_cli_auth_required"]) is not bool or not tcp_cli_auth_configurable):
        raise ValueError("TCP CLI authentication setting requires wireless status v3")
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
    if tcp_cli_configurable:
        payload[4] |= 16 if "tcp_cli_enabled" not in settings else 8 if not settings["tcp_cli_enabled"] else 0
    if tcp_cli_auth_configurable:
        if "tcp_cli_auth_required" in settings:
            if settings["tcp_cli_auth_required"]:
                payload[4] |= 32
        elif "ble_key" in settings and settings["ble_key"] == "":
            payload[4] |= 32
        else:
            payload[4] |= 64
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
        if key != "":  # an empty key erases the saved key; the payload field stays zero
            if not isinstance(key, str) or len(key) != 64 or any(
                    char not in "0123456789abcdefABCDEF" for char in key):
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

WIRELESS_TEST_DETAILS = ("", "Searching for network", "Checking password", "Waiting for IP address",
                         "Network not found", "Password rejected", "Connected but no IP address from DHCP",
                         "Timed out", "Could not start connection", "Test connection lost", "Test credentials expired")

def parse_wireless_result_report(report: bytes | bytearray | list[int]) -> WirelessOperation:
    """Decode sanitized Feature 35 scan/test results; no password is returned."""
    payload = _feature_payload(report, WIRELESS_RESULT_REPORT_ID, WIRELESS_RESULT_FEATURE_SIZE - 1)
    state, page, count, wifi_state = payload[2:6]
    max_page = (8 if payload[0] == 1 else len(WIRELESS_TEST_DETAILS) - 1) if state >= 4 else 2
    if payload[0] not in (1, 2) or state > 6 or page > max_page or count > 18 or wifi_state > 3:
        raise ValueError("Invalid wireless scan/test result")
    networks = []
    for index in range(6):
        entry = payload[6 + index * 35: 41 + index * 35]
        length = entry[0]
        if length == 0:
            if any(entry):
                raise ValueError("Invalid empty wireless scan entry")
            continue
        if length > 32 or any(entry[1 + length:33]) or entry[34] > (1 if payload[0] == 1 else 7):
            raise ValueError("Invalid wireless scan entry")
        try:
            ssid = entry[1:1 + length].decode("utf-8")
        except UnicodeError:
            raise ValueError("Invalid wireless scan SSID") from None
        networks.append({"ssid": ssid, "rssi": int.from_bytes(entry[33:34], "little", signed=True),
                         "secured": bool(entry[34]),
                         "security": (("Secured" if entry[34] else "Open") if payload[0] == 1 else
                                      ("Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)",
                                       "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN")[entry[34]])})
    remaining_seconds = int.from_bytes(payload[220:222], "little")
    staged = state == 5 or state == 6 and page == 9
    if len(networks) > count or (state != 5 and any(payload[216:220])) or remaining_seconds > 600:
        raise ValueError("Invalid wireless scan/test result fields")
    if not staged and remaining_seconds:
        raise ValueError("Staged credential lifetime requires joined or link-lost test credentials")
    detail = page if state >= 4 else 0
    return {"transaction_id": payload[1], "state": state, "page": 0 if state >= 4 else page, "detail": detail,
            "phase": WIRELESS_TEST_DETAILS[detail], "count": count, "wifi_state": wifi_state,
            "ipv4": ".".join(map(str, payload[216:220])), "networks": networks,
            "remaining_seconds": remaining_seconds if staged else 0}

def parse_wireless_status_report(report: bytes | bytearray | list[int]) -> WirelessStatus:
    """Decode sanitized Feature 30; credentials are never present in this report."""
    data = _bytes(report)
    offset = 2 if len(data) >= 2 and data[:2] == bytes((0, WIRELESS_REPORT_ID)) else 1
    if offset >= len(data) or data[0] != 0 and data[0] != WIRELESS_REPORT_ID:
        raise ValueError("Invalid wireless status report")
    payload_size = 53 if data[offset] >= 4 else 52
    payload = _feature_payload(data, WIRELESS_REPORT_ID, payload_size)
    restart_flag = payload[52] if payload_size == 53 else 0
    if (payload[0] not in (1, 2, 3, 4) or payload[1] & ~3 or payload[2] > 6 or payload[4] > 3
            or payload[42] > 1 or payload[43] > 1 or payload[44] > 3 or payload[45] > 2
            or payload[46] > 1 or payload[47] > (0 if payload[0] == 1 else 1 if payload[0] == 2 else 3)
            or payload[0] < 3 and any(payload[48:52]) or payload[0] >= 3 and payload[48] > 3
            or payload[0] < 4 and restart_flag != 0 or restart_flag > 1):
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
        "error": payload[44], "ble_state": ("off", "advertising", "connected")[payload[45]],
        "tcp_cli_started": bool(payload[46]),
        "tcp_cli_enabled": not bool(payload[47] & 1), "tcp_cli_configurable": payload[0] >= 2,
        "tcp_cli_auth_required": bool(payload[47] & 2), "tcp_cli_auth_configurable": payload[0] >= 3,
        "wifi_auth_state": (None if payload[0] < 3 else
                            ("try_wpa2", "try_sae", "wpa2", "sae")[payload[48]]),
        "reconnect_count": int.from_bytes(payload[49:51], "little") if payload[0] >= 3 else 0,
        "rssi": (int.from_bytes(payload[51:52], "little", signed=True)
                 if payload[0] >= 3 and payload[51] != 127 else None),
        "restart_required": bool(payload[0] >= 4 and restart_flag),
    }
