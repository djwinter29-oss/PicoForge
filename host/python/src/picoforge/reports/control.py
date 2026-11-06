# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""USB Feature 20 command and result codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .models import BootSettings, ControlResult, SupportedHardwareIdentity, WirelessNetwork

from .common import (
    CAP_LED, CAP_REBOOT, CAP_SAVE_SETTINGS, CAP_SELF_TEST, CAP_WIRELESS,
    CONTROL_ERROR_NAMES, CONTROL_PAYLOAD_SIZE, CONTROL_REPORT_ID, CONTROL_STATUS_NAMES, REPORT_VERSION,
    ControlDetail, ControlError, ControlOpcode, ControlStatus, InvalidRequest, ProtocolError,
    _ALLOWED_STATUS_ERRORS, _OPCODE_STATUSES, _bytes, _feature_payload, _validate_settings,
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
        transaction_id, ControlOpcode.SAVE_SETTINGS, flags=int(force) | (int(commit) << 1),
        arguments=struct.pack("<HBB", clock, profile, radios | (int(tcp) << 2)),
    )


def build_control_command_report(
    transaction_id: int, opcode: int, *, flags: int = 0, arguments: bytes | bytearray = b"",
) -> bytes:
    """Build one 69-byte HIDAPI SET, padding only unused arguments."""
    if type(transaction_id) is not int or not 1 <= transaction_id <= 255:
        raise ValueError("transaction ID must be an integer from 1 to 255")
    if isinstance(opcode, ControlOpcode):
        opcode = int(opcode)
    if type(opcode) is not int:
        raise ValueError("control opcode is unsupported")
    try:
        opcode = ControlOpcode(opcode)
    except ValueError:
        raise ValueError("control opcode is unsupported") from None
    if type(flags) is not int or not 0 <= flags <= 255:
        raise ValueError("flags must be a byte")
    args = _bytes(arguments)
    if len(args) > 64:
        raise ValueError("control command arguments exceed 64 bytes")
    args = args.ljust(64, b"\0")
    # Keep this final wire check as the guard for every direct builder caller before a HID write.
    if opcode == ControlOpcode.LED and (flags or args[0] not in (0, 1) or any(args[1:])):
        raise ValueError("invalid LED command fields")
    if opcode == ControlOpcode.REBOOT and (flags != 1 or any(args)):
        raise ValueError("reboot requires confirmation and zero arguments")
    if opcode == ControlOpcode.MSC and (flags & ~1 or args[0] not in (0, 1, 2) or any(args[1:])
                        or (args[0] == 2) != bool(flags & 1)):
        raise ValueError("invalid MSC command fields or force-return confirmation")
    if opcode == ControlOpcode.SELF_TEST:
        size = int.from_bytes(args[1:5], "little")
        if (flags or any(args[5:]) or args[0] not in (0, 1)
                or (args[0] == 0 and size) or (args[0] == 1 and not 1 <= size <= 1024)):
            raise ValueError("invalid self-test command fields")
    if opcode == ControlOpcode.SAVE_SETTINGS:
        clock, profile, switches = struct.unpack_from("<HBB", args)
        if flags & ~3 or switches & ~7 or clock not in (0, 150, 200, 250) or profile not in (0, 1) or any(args[4:]):
            raise ValueError("invalid persistent settings command fields")
    if opcode >= ControlOpcode.STAGE_SSID:
        if flags:
            raise ValueError("wireless flags must be zero")
        if opcode in (ControlOpcode.STAGE_SSID, ControlOpcode.STAGE_KEY) and (
                args[0] > 32 or any(args[1 + args[0]:])
                or (opcode == ControlOpcode.STAGE_KEY and args[0] not in (0, 32))):
            raise ValueError("invalid wireless value length or padding")
        if opcode == ControlOpcode.STAGE_PASSWORD and (
                args[0] not in (0, *range(8, 64)) or any(args[1 + args[0]:])
                             or any(not 32 <= value <= 126 for value in args[1:1 + args[0]])):
            raise ValueError("invalid password length, characters or padding")
        if opcode in (ControlOpcode.WIFI_TEST, ControlOpcode.WIFI_CANCEL, ControlOpcode.WIFI_SCAN) and any(args):
            raise ValueError("wireless command requires zero arguments")
        if opcode == ControlOpcode.WIFI_ENTRY and (
                args[0] >= 18 or not int.from_bytes(args[1:3], "little") or any(args[3:])):
            raise ValueError("invalid scan index or generation")
        if opcode == ControlOpcode.TCP_PROOF and any(args[16:]):
            raise ValueError("invalid proof request padding")
    return bytes((CONTROL_REPORT_ID, REPORT_VERSION, transaction_id, opcode, flags)) + args


def _validate_detail(opcode: int, status: int, error: int, detail: int) -> None:
    """Validate the global detail namespace and its opcode/status applicability."""
    allowed = {ControlDetail.NONE}
    if status == ControlStatus.INVALID and error == ControlError.ARGUMENT:
        if opcode == ControlOpcode.SELF_TEST:
            allowed = {ControlDetail.INVALID_BENCHMARK_SIZE}
        elif opcode == ControlOpcode.STAGE_SSID:
            allowed = {ControlDetail.SSID_LENGTH_INVALID}
        elif opcode == ControlOpcode.STAGE_PASSWORD:
            allowed = {ControlDetail.PASSWORD_INVALID}
        elif opcode == ControlOpcode.STAGE_KEY:
            allowed = {ControlDetail.CONTROL_KEY_LENGTH_INVALID}
        elif opcode in (ControlOpcode.SAVE_SETTINGS, ControlOpcode.WIFI_TEST):
            allowed |= {
                ControlDetail.CREDENTIAL_PAIR_INCOMPLETE,
                ControlDetail.STAGED_VALUES_EXPIRED,
                ControlDetail.NO_ELIGIBLE_STAGED_VALUES,
            }
            if opcode == ControlOpcode.SAVE_SETTINGS:
                allowed.add(ControlDetail.STAGED_CREDENTIAL_COMMIT_REQUIRED)
        elif opcode == ControlOpcode.WIFI_ENTRY:
            allowed |= {ControlDetail.SCAN_GENERATION_STALE, ControlDetail.SCAN_INDEX_OUT_OF_RANGE}
        elif opcode == ControlOpcode.TCP_PROOF:
            allowed = {ControlDetail.CONTROL_KEY_NOT_SAVED}
    elif status == ControlStatus.BUSY:
        allowed = set()
        if opcode in (ControlOpcode.REBOOT, ControlOpcode.MSC, ControlOpcode.SELF_TEST,
                      ControlOpcode.SAVE_SETTINGS):
            allowed |= {ControlDetail.CONFLICTING_WORK, ControlDetail.USB_MSC_OWNS_STORAGE}
        if opcode in (ControlOpcode.SAVE_SETTINGS, ControlOpcode.STAGE_SSID, ControlOpcode.STAGE_PASSWORD,
                      ControlOpcode.STAGE_KEY, ControlOpcode.WIFI_TEST, ControlOpcode.WIFI_SCAN):
            allowed.add(ControlDetail.WIFI_TEST_ACTIVE)
        if opcode in (ControlOpcode.SAVE_SETTINGS, ControlOpcode.WIFI_TEST, ControlOpcode.WIFI_SCAN,
                      ControlOpcode.WIFI_ENTRY):
            allowed.add(ControlDetail.WIFI_SCAN_ACTIVE)
    elif status == ControlStatus.FAILED and opcode == ControlOpcode.SELF_TEST:
        allowed = {
            ControlDetail.CARD_UNAVAILABLE,
            ControlDetail.INSUFFICIENT_SPACE,
            ControlDetail.BENCHMARK_FILE_EXISTS,
            ControlDetail.PREFLIGHT_FILESYSTEM_IO,
            ControlDetail.FILESYSTEM_NOT_MOUNTED,
        }
    if detail not in allowed:
        raise ValueError("invalid HID control detail for opcode/status/error")


def parse_control_result_report(
    report: bytes | bytearray | list[int], identity: SupportedHardwareIdentity | None = None,
) -> ControlResult:
    """Decode and validate the latest USB result without consuming it."""
    payload = _feature_payload(report, CONTROL_REPORT_ID, CONTROL_PAYLOAD_SIZE)
    version, transaction, opcode, status_value, error_value = payload[:5]
    data = payload[5:]
    if version != REPORT_VERSION:
        raise ProtocolError(f"unsupported HID control report version {version}")
    try:
        status = ControlStatus(status_value)
        error = ControlError(error_value)
    except ValueError:
        raise ValueError("invalid HID control status/error combination") from None
    try:
        opcode_enum = ControlOpcode(opcode)
    except ValueError:
        opcode_enum = None
    if status not in _ALLOWED_STATUS_ERRORS or error not in _ALLOWED_STATUS_ERRORS[status]:
        raise ValueError("invalid HID control status/error combination")
    if status == ControlStatus.IDLE:
        if transaction or opcode or any(data):
            raise ValueError("invalid nonempty HID control idle result")
    elif not transaction:
        raise ValueError("HID control result has a zero transaction ID")
    if opcode_enum is not None:
        if status not in _OPCODE_STATUSES[opcode_enum]:
            raise ValueError("HID control status is invalid for its opcode")
        if status == ControlStatus.INVALID and error not in (
                {ControlError.MALFORMED, ControlError.CONFIRMATION}
                    if opcode_enum == ControlOpcode.REBOOT else
                {ControlError.MALFORMED, ControlError.CONFIRMATION, ControlError.ARGUMENT}
                    if opcode_enum == ControlOpcode.MSC else
                {ControlError.MALFORMED, ControlError.ARGUMENT}):
            raise ValueError("HID control invalid error is not valid for its opcode")
        if status == ControlStatus.UNSUPPORTED and error != ControlError.CAPABILITY:
            raise ValueError("invalid unsupported result for assigned HID opcode")
    elif status != ControlStatus.IDLE and (
            status != ControlStatus.UNSUPPORTED or error != ControlError.OPCODE):
        raise ValueError("unknown HID opcode must be unsupported")
    if status == ControlStatus.UNSUPPORTED and error == ControlError.CAPABILITY and identity is not None:
        required = {
            ControlOpcode.LED: CAP_LED,
            ControlOpcode.REBOOT: CAP_REBOOT,
            ControlOpcode.SELF_TEST: CAP_SELF_TEST,
            ControlOpcode.SAVE_SETTINGS: CAP_SAVE_SETTINGS,
        }.get(opcode_enum) if opcode_enum is not None else None
        if opcode_enum is not None and opcode_enum >= ControlOpcode.STAGE_SSID:
            required = CAP_WIRELESS
        capabilities = identity.get("capabilities")
        if opcode_enum == ControlOpcode.MSC or (
                required is not None and capabilities is not None and capabilities & required):
            raise ValueError("HID control result contradicts advertised capabilities")
    saved: BootSettings | None = None
    scan_entry: WirelessNetwork | None = None
    proof = None
    commit_mask = 0
    detail = ControlDetail.NONE
    if status in (ControlStatus.INVALID, ControlStatus.BUSY, ControlStatus.FAILED):
        detail = data[0]
        _validate_detail(opcode, status, error, detail)
        if any(data[1:]):
            raise ValueError("nonzero control error padding")
    elif status == ControlStatus.COMPLETED and opcode == ControlOpcode.SAVE_SETTINGS:
        clock, profile, switches, commit_mask = struct.unpack_from("<HBBB", data)
        saved = {"system_clock_mhz": clock, "sdcard_profile": profile, "radios": switches & 3,
                 "tcp_cli_enabled": bool(switches & 4)}
        try:
            _validate_settings(saved)
        except InvalidRequest as error:
            raise ValueError(f"invalid HID settings result: {error}") from None
        if switches & ~7 or commit_mask & ~7 or any(data[5:]):
            raise ValueError("invalid HID settings result data")
    elif status == ControlStatus.COMPLETED and opcode == ControlOpcode.WIFI_ENTRY:
        from .wireless import parse_scan_entry
        scan_entry = parse_scan_entry(data)
    elif status == ControlStatus.COMPLETED and opcode == ControlOpcode.TCP_PROOF:
        if any(data[16:]):
            raise ValueError("invalid proof result padding")
        proof = data[:16]
    elif any(data):
        raise ValueError("unexpected HID control result data")
    return {
        "version": version, "transaction_id": transaction, "opcode": opcode,
        "status_id": status, "status": CONTROL_STATUS_NAMES[status],
        "error_reason": error, "error": CONTROL_ERROR_NAMES[error], "detail": detail,
        "saved_settings": saved, "commit_mask": commit_mask, "scan_entry": scan_entry, "proof": proof,
    }


def build_led_command_report(on: bool, transaction_id: int = 1) -> bytes:
    if type(on) is not bool:
        raise ValueError("LED state must be boolean")
    return build_control_command_report(transaction_id, ControlOpcode.LED, arguments=bytes((int(on),)))


def build_msc_command_report(enable: bool, *, force: bool = False, transaction_id: int = 1) -> bytes:
    if type(enable) is not bool or type(force) is not bool or (force and enable):
        raise ValueError("Force return requires disabling MSC")
    return build_control_command_report(transaction_id, ControlOpcode.MSC, flags=int(force),
                                        arguments=bytes((2 if force else int(enable),)))
