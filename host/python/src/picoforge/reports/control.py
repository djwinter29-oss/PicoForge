"""Control HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import ControlResult, HardwareIdentity

from .common import (
    CAP_LED,
    CAP_REBOOT,
    CAP_SAVE_SETTINGS,
    CAP_SELF_TEST,
    CONTROL_COMPLETED,
    CONTROL_ERROR_NAMES,
    CONTROL_IDLE,
    CONTROL_INVALID,
    CONTROL_PAYLOAD_SIZE,
    CONTROL_REPORT_ID,
    CONTROL_STATUS_NAMES,
    CONTROL_UNSUPPORTED,
    REPORT_VERSION,
)
from .common import (
    _ALLOWED_STATUS_ERRORS,
    _OPCODE_STATUSES,
    _bytes,
    _feature_payload,
    _validate_settings,
)


def build_settings_command_report(
    settings: dict[str, object], transaction_id: int = 1, *, force: bool = False,
    tcp_cli_enabled: bool | None = None,
) -> bytes:
    """Build Feature report 20 opcode 6 to save persistent settings."""
    clock, profile, radios = _validate_settings(settings)
    if type(force) is not bool:
        raise ValueError("Forced settings save confirmation must be boolean")
    if tcp_cli_enabled is not None and type(tcp_cli_enabled) is not bool:
        raise ValueError("TCP CLI setting must be boolean")
    arguments = struct.pack("<HBB", clock, profile, radios) + bytes((int(tcp_cli_enabled or False),))
    flags = int(force) | (2 if tcp_cli_enabled is not None else 0)
    return build_control_command_report(transaction_id, 6, flags=flags, arguments=arguments)

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
        set_tcp_cli = bool(flags & 2)
        if (flags & ~3 or args[3] > 3 or (args[4] > 1 if set_tcp_cli else args[4] != 0)
                or clock not in (0, 150, 200, 250) or profile not in (0, 1)):
            raise ValueError("invalid persistent settings command fields")
    if opcode not in (1, 2, 3, 4, 6):
        raise ValueError("control opcode is unsupported")
    return bytes((CONTROL_REPORT_ID, REPORT_VERSION, transaction_id, opcode, flags)) + args

def parse_control_result_report(
    report: bytes | bytearray | list[int], identity: HardwareIdentity | None = None
) -> ControlResult:
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
        clock, profile, result_flags = struct.unpack("<HBB", result_data)
        radios = result_flags & 3
        tcp_cli_included = bool(result_flags & 8)
        tcp_cli_enabled = bool(result_flags & 4)
        if result_flags & 0xF0 or (tcp_cli_enabled and not tcp_cli_included) or (
            status == CONTROL_COMPLETED and (clock not in (0, 150, 200, 250) or profile not in (0, 1))
        ):
            raise ValueError("invalid HID settings result data")
        if status != CONTROL_COMPLETED and any(result_data):
            raise ValueError("unsuccessful settings command returned result data")
        if status == CONTROL_COMPLETED:
            saved_settings = {"system_clock_mhz": clock, "sdcard_profile": profile, "radios": radios}
            if tcp_cli_included:
                saved_settings["tcp_cli_enabled"] = tcp_cli_enabled
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
