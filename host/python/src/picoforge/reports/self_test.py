"""Self test HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import SelfTestStatus

from .common import (
    REPORT_VERSION,
    SELF_TEST_PAYLOAD_SIZE,
    SELF_TEST_REPORT_ID,
)
from .common import _input_payload, _unknown_name
from .control import build_control_command_report


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

def parse_self_test_report(report: bytes | bytearray | list[int]) -> SelfTestStatus:
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


parse_self_test_status_report = parse_self_test_report
