"""Storage HID report codecs."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..hid.models import HardwareIdentity, MemoryStatus

from .common import (
    REPORT_VERSION,
    STORAGE_PAYLOAD_SIZE,
    STORAGE_REPORT_ID,
)
from .common import _input_payload


def parse_memory_report(
    report: bytes | bytearray | list[int],
    identity: HardwareIdentity | None = None,
) -> MemoryStatus:
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
        },
    }
