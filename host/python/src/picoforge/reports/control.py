"""USB Feature 20 command and result codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import ControlResult, HardwareIdentity

from .common import (
    CAP_LED, CAP_REBOOT, CAP_SAVE_SETTINGS, CAP_SELF_TEST, CAP_WIRELESS,
    CONTROL_ERROR_NAMES, CONTROL_PAYLOAD_SIZE, CONTROL_REPORT_ID, CONTROL_STATUS_NAMES, REPORT_VERSION,
    _ALLOWED_STATUS_ERRORS, _OPCODE_STATUSES, InvalidRequest, _bytes, _feature_payload, _validate_settings,
)


def build_settings_command_report(
    settings: dict[str, object], transaction_id: int = 1, *, force: bool = False,
    tcp_cli_enabled: bool | None = None, commit: bool = False,
) -> bytes:
    """Build opcode 5 with all three saved switches explicitly supplied."""
    clock, profile, radios = _validate_settings(settings)
    if type(force) is not bool or type(commit) is not bool:
        raise ValueError("Save confirmations must be boolean")
    tcp = settings.get("tcp_cli_enabled") if tcp_cli_enabled is None else tcp_cli_enabled
    if type(tcp) is not bool:
        raise ValueError("The saved TCP CLI switch is required")
    return build_control_command_report(
        transaction_id, 5, flags=int(force) | (int(commit) << 1),
        arguments=struct.pack("<HBB", clock, profile, radios | (int(tcp) << 2)),
    )


def build_control_command_report(
    transaction_id: int, opcode: int, *, flags: int = 0, arguments: bytes | bytearray = b"",
) -> bytes:
    """Build one 69-byte HIDAPI SET, padding only unused arguments."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("transaction ID must be an integer from 1 to 255")
    if type(opcode) is not int or opcode not in range(1, 14):
        raise ValueError("control opcode is unsupported")
    if type(flags) is not int or not 0 <= flags <= 255:
        raise ValueError("flags must be a byte")
    args = _bytes(arguments)
    if len(args) > 64:
        raise ValueError("control command arguments exceed 64 bytes")
    args = args.ljust(64, b"\0")
    if opcode == 1 and (flags or args[0] not in (0, 1) or any(args[1:])):
        raise ValueError("invalid LED command fields")
    if opcode == 2 and (flags != 1 or any(args)):
        raise ValueError("reboot requires confirmation and zero arguments")
    if opcode == 3 and (flags & ~1 or args[0] not in (0, 1, 2) or any(args[1:])
                        or (args[0] == 2) != bool(flags & 1)):
        raise ValueError("invalid MSC command fields or force-return confirmation")
    if opcode == 4:
        size = int.from_bytes(args[1:5], "little")
        if (flags or any(args[5:]) or args[0] not in (0, 1)
                or (args[0] == 0 and size) or (args[0] == 1 and not 1 <= size <= 1024)):
            raise ValueError("invalid self-test command fields")
    if opcode == 5:
        clock, profile, switches = struct.unpack_from("<HBB", args)
        if flags & ~3 or switches & ~7 or clock not in (0, 150, 200, 250) or profile not in (0, 1) or any(args[4:]):
            raise ValueError("invalid persistent settings command fields")
    if opcode >= 6:
        if flags:
            raise ValueError("wireless flags must be zero")
        if opcode in (6, 8) and (args[0] > 32 or any(args[1 + args[0]:])
                                  or (opcode == 8 and args[0] not in (0, 32))):
            raise ValueError("invalid wireless value length or padding")
        if opcode == 7 and (args[0] not in (0, *range(8, 64)) or any(args[1 + args[0]:])
                             or any(not 32 <= value <= 126 for value in args[1:1 + args[0]])):
            raise ValueError("invalid password length, characters or padding")
        if opcode in (9, 10, 11) and any(args):
            raise ValueError("wireless command requires zero arguments")
        if opcode == 12 and (args[0] >= 18 or not int.from_bytes(args[1:3], "little") or any(args[3:])):
            raise ValueError("invalid scan index or generation")
        if opcode == 13 and any(args[16:]):
            raise ValueError("invalid proof request padding")
    return bytes((CONTROL_REPORT_ID, REPORT_VERSION, transaction_id, opcode, flags)) + args


def _validate_detail(opcode: int, status: int, error: int, detail: int) -> None:
    """Validate the global detail namespace and its opcode/status applicability."""
    allowed = {0}
    if status == 3 and error == 3:
        if opcode == 4:
            allowed = {3}
        elif opcode in (6, 7, 8):
            allowed = {opcode + 10}
        elif opcode in (5, 9):
            allowed |= {19, 20, 21}
            if opcode == 5:
                allowed.add(27)
        elif opcode == 12:
            allowed |= {22, 23}
        elif opcode == 13:
            allowed = {24}
    elif status == 5:
        allowed = set()
        if opcode in (2, 3, 4, 5):
            allowed |= {1, 2}
        if opcode in (5, 6, 7, 8, 9, 11):
            allowed.add(25)
        if opcode in (5, 9, 11, 12):
            allowed.add(26)
    elif status == 6 and opcode == 4:
        allowed = {4, 5, 6, 7, 10}
    if detail not in allowed:
        raise ValueError("invalid HID control detail for opcode/status/error")


def parse_control_result_report(
    report: bytes | bytearray | list[int], identity: HardwareIdentity | None = None,
) -> ControlResult:
    """Decode and validate the latest USB result without consuming it."""
    payload = _feature_payload(report, CONTROL_REPORT_ID, CONTROL_PAYLOAD_SIZE)
    version, transaction, opcode, status, error = payload[:5]
    data = payload[5:]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID control report version {version}")
    if status not in _ALLOWED_STATUS_ERRORS or error not in _ALLOWED_STATUS_ERRORS[status]:
        raise ValueError("invalid HID control status/error combination")
    if status == 0:
        if transaction or opcode or any(data):
            raise ValueError("invalid nonempty HID control idle result")
    elif not transaction:
        raise ValueError("HID control result has a zero transaction ID")
    if opcode in _OPCODE_STATUSES:
        if status not in _OPCODE_STATUSES[opcode]:
            raise ValueError("HID control status is invalid for its opcode")
        if status == 3 and error not in ({1, 2} if opcode == 2 else {1, 2, 3} if opcode == 3 else {1, 3}):
            raise ValueError("HID control invalid error is not valid for its opcode")
        if status == 4 and error != 4:
            raise ValueError("invalid unsupported result for assigned HID opcode")
    elif status != 0 and (status != 4 or error != 7):
        raise ValueError("unknown HID opcode must be unsupported")
    if status == 4 and error == 4 and identity is not None:
        required = {1: CAP_LED, 2: CAP_REBOOT, 4: CAP_SELF_TEST, 5: CAP_SAVE_SETTINGS}.get(opcode)
        if opcode >= 6:
            required = CAP_WIRELESS
        if opcode == 3 or (required is not None and identity["capabilities"] & required):
            raise ValueError("HID control result contradicts advertised capabilities")
    saved = None
    scan_entry = None
    proof = None
    commit_mask = 0
    detail = 0
    if status in (3, 5, 6):
        detail = data[0]
        _validate_detail(opcode, status, error, detail)
        if any(data[1:]):
            raise ValueError("nonzero control error padding")
    elif status == 2 and opcode == 5:
        clock, profile, switches, commit_mask = struct.unpack_from("<HBBB", data)
        saved = {"system_clock_mhz": clock, "sdcard_profile": profile, "radios": switches & 3,
                 "tcp_cli_enabled": bool(switches & 4)}
        try:
            _validate_settings(saved)
        except InvalidRequest as error:
            raise ValueError(f"invalid HID settings result: {error}") from None
        if switches & ~7 or commit_mask & ~7 or any(data[5:]):
            raise ValueError("invalid HID settings result data")
    elif status == 2 and opcode == 12:
        from .wireless import parse_scan_entry
        scan_entry = parse_scan_entry(data)
    elif status == 2 and opcode == 13:
        if any(data[16:]):
            raise ValueError("invalid proof result padding")
        proof = data[:16]
    elif any(data):
        raise ValueError("unexpected HID control result data")
    return {
        "version": version, "transaction_id": transaction, "opcode": opcode,
        "status_id": status, "status": CONTROL_STATUS_NAMES[status],
        "error_reason": error, "error": CONTROL_ERROR_NAMES[error],
        "result_data": int.from_bytes(data, "little"), "detail": detail,
        "saved_settings": saved, "commit_mask": commit_mask, "scan_entry": scan_entry, "proof": proof,
    }


def build_led_command_report(on: bool, transaction_id: int = 1) -> bytes:
    if type(on) is not bool:
        raise ValueError("LED state must be boolean")
    return build_control_command_report(transaction_id, 1, arguments=bytes((int(on),)))


def build_msc_command_report(enable: bool, *, force: bool = False, transaction_id: int = 1) -> bytes:
    if type(enable) is not bool or type(force) is not bool or (force and enable):
        raise ValueError("Force return requires disabling MSC")
    return build_control_command_report(transaction_id, 3, flags=int(force),
                                        arguments=bytes((2 if force else int(enable),)))
