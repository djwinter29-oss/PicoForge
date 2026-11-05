"""Wireless HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import WirelessOperation, WirelessStatus

from .common import (
    CONTROL_STATUS_NAMES,
    REPORT_VERSION,
    TCP_CLI_AUTH_REPORT_ID,
    TCP_CLI_AUTH_REQUEST_SIZE,
    TCP_CLI_AUTH_RESULT_SIZE,
    WIRELESS_COMMAND_FEATURE_SIZE,
    WIRELESS_COMMAND_REPORT_ID,
    WIRELESS_REPORT_ID,
    WIRELESS_RESULT_FEATURE_SIZE,
    WIRELESS_RESULT_REPORT_ID,
)
from .common import _feature_payload, _validate_settings

WIRELESS_STATUS_SIZE = 53


def build_tcp_cli_auth_request(transaction_id: int, nonce: bytes) -> bytes:
    """Build the USB-only request for a board-bound TCP CLI proof."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("TCP CLI auth transaction must be 1..255")
    if not isinstance(nonce, bytes) or len(nonce) != 16:
        raise ValueError("TCP CLI auth nonce must contain exactly 16 bytes")
    return bytes((TCP_CLI_AUTH_REPORT_ID, REPORT_VERSION, transaction_id)) + nonce + b"\0"


def parse_tcp_cli_auth_result(report: bytes | bytearray | list[int], transaction_id: int) -> bytes:
    """Validate the matching USB TCP CLI proof result and return its 16-byte proof."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("TCP CLI auth transaction must be 1..255")
    payload = _feature_payload(report, TCP_CLI_AUTH_REPORT_ID, TCP_CLI_AUTH_RESULT_SIZE - 1)
    if payload[0] != REPORT_VERSION or payload[1] != transaction_id or payload[2] not in (0, 1):
        raise ValueError("invalid or stale TCP CLI auth result")
    proof = payload[3:]
    if payload[2] != 1:
        raise RuntimeError("The board cannot create a TCP CLI proof; check the saved Control key and firmware")
    return proof


def build_wireless_configuration_report(settings: dict[str, object], transaction_id: int = 1) -> bytes:
    """Build Feature 31: board settings, radio and TCP CLI switches and credentials in one atomic save."""
    if not isinstance(settings, dict):
        raise ValueError("Expected wireless configuration JSON")
    if settings.get("confirm_reboot") is not True:
        raise ValueError("Explicit confirm_reboot: true is required")
    if set(settings) - {"wifi_enabled", "ble_enabled", "ssid", "password", "ble_key", "confirm_reboot", "board",
                       "use_tested_wifi", "tcp_cli_enabled"}:
        raise ValueError("Unknown wireless configuration field")
    if type(settings.get("wifi_enabled")) is not bool or type(settings.get("ble_enabled")) is not bool:
        raise ValueError("wifi_enabled and ble_enabled must be boolean")
    if "use_tested_wifi" in settings and type(settings["use_tested_wifi"]) is not bool:
        raise ValueError("use_tested_wifi must be boolean")
    use_tested = settings.get("use_tested_wifi", False)
    if type(settings.get("tcp_cli_enabled")) is not bool:
        raise ValueError("tcp_cli_enabled must be boolean")
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
    payload[:5] = bytes([31, REPORT_VERSION, transaction_id, radios, 0 if settings["tcp_cli_enabled"] else 8])
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
                raise ValueError("Control key must contain 64 hexadecimal characters and be nonzero")
            decoded = bytes.fromhex(key)
            if not any(decoded):
                raise ValueError("Control key must contain 64 hexadecimal characters and be nonzero")
            payload[102:134] = decoded
    if use_tested:
        payload[4] |= 4
    return bytes(payload)

def validate_wireless_configuration(settings: object) -> None:
    """Validate a dashboard save request, where an omitted TCP CLI switch keeps the saved value."""
    if not isinstance(settings, dict):
        raise ValueError("Expected wireless configuration JSON")
    build_wireless_configuration_report({"tcp_cli_enabled": True, **settings})


def build_wireless_command_report(
    transaction_id: int, operation: int, *, page: int = 0, ssid: str = "", password: str = ""
) -> bytes:
    """Build USB-only wireless scan, paging, test-join or staged-credential clear command."""
    if (type(transaction_id) is not int or not 1 <= transaction_id <= 255
            or type(operation) is not int or operation not in (1, 2, 3, 4)):
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
    max_page = len(WIRELESS_TEST_DETAILS) - 1 if state >= 4 else 2
    if payload[0] != REPORT_VERSION or state > 6 or page > max_page or count > 18 or wifi_state > 3:
        raise ValueError("Invalid wireless scan/test result")
    networks = []
    for index in range(6):
        entry = payload[6 + index * 35: 41 + index * 35]
        length = entry[0]
        if length == 0:
            if any(entry):
                raise ValueError("Invalid empty wireless scan entry")
            continue
        if length > 32 or any(entry[1 + length:33]) or entry[34] > 7:
            raise ValueError("Invalid wireless scan entry")
        try:
            ssid = entry[1:1 + length].decode("utf-8")
        except UnicodeError:
            raise ValueError("Invalid wireless scan SSID") from None
        networks.append({"ssid": ssid, "rssi": int.from_bytes(entry[33:34], "little", signed=True),
                         "secured": bool(entry[34]),
                         "security": ("Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)",
                                      "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN")[entry[34]]})
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
    payload = _feature_payload(report, WIRELESS_REPORT_ID, WIRELESS_STATUS_SIZE)
    if (payload[0] != REPORT_VERSION or payload[1] & ~3 or payload[2] > 6 or payload[4] > 3
            or payload[42] > 1 or payload[43] > 1 or payload[44] > 3 or payload[45] > 2
            or payload[46] > 1 or payload[47] > 3 or payload[48] > 3 or payload[52] > 1):
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
        "tcp_cli_enabled": not bool(payload[47] & 1), "tcp_cli_auth_required": bool(payload[47] & 2),
        "wifi_auth_state": ("try_wpa2", "try_sae", "wpa2", "sae")[payload[48]],
        "reconnect_count": int.from_bytes(payload[49:51], "little"),
        "rssi": int.from_bytes(payload[51:52], "little", signed=True) if payload[51] != 127 else None,
        "restart_required": bool(payload[52]),
    }
