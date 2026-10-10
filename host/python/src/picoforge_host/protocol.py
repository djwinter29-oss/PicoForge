"""Decoders for the PicoForge HID status protocol."""

from __future__ import annotations

import struct

INFO_REPORT_ID = 1
STATUS_REPORT_ID = 2
REPORT_VERSION = 1
INFO_PAYLOAD_SIZE = 26
STATUS_PAYLOAD_SIZE = 7
PRODUCT_NAME_SIZE = 20
INFO_FEATURE_SIZE = INFO_PAYLOAD_SIZE + 1
STATUS_INPUT_SIZE = STATUS_PAYLOAD_SIZE + 1

MCU_NAMES = {1: "RP2040", 2: "RP2350"}
STATUS_USB_MOUNTED = 1 << 0
STATUS_CDC_OPEN = 1 << 1
STATUS_KNOWN_FLAGS = STATUS_USB_MOUNTED | STATUS_CDC_OPEN


def _feature_payload(report: bytes | bytearray | list[int], report_id: int, size: int) -> bytes:
    data = bytes(report)
    if len(data) >= size + 2 and data[0] == 0 and data[1] == report_id:
        offset = 2
    elif len(data) >= size + 1 and data[0] == report_id:
        offset = 1
    else:
        raise ValueError(f"unexpected HID feature report {report_id} length or ID")
    payload = data[offset : offset + size]
    if len(payload) != size or any(data[offset + size :]):
        raise ValueError(f"unexpected HID feature report {report_id} length or padding")
    return payload


def parse_info_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode a report-ID-prefixed hardware/product Feature report."""
    payload = _feature_payload(report, INFO_REPORT_ID, INFO_PAYLOAD_SIZE)
    version, mcu_id, system_clock_hz = struct.unpack_from("<BBI", payload)
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID info report version {version}")
    if system_clock_hz == 0:
        raise ValueError("invalid system clock in HID info report")
    try:
        product_name = payload[6 : 6 + PRODUCT_NAME_SIZE].split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("invalid product name in HID info report") from error
    if not product_name:
        raise ValueError("empty product name in HID info report")
    return {
        "version": version,
        "mcu_id": mcu_id,
        "mcu": MCU_NAMES.get(mcu_id, f"Unknown MCU ({mcu_id})"),
        "system_clock_hz": system_clock_hz,
        "product_name": product_name,
    }


def parse_status_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode a report-ID-prefixed periodic HID Input report."""
    data = bytes(report)
    if len(data) < STATUS_INPUT_SIZE or data[0] != STATUS_REPORT_ID:
        raise ValueError("unexpected HID status report length or ID")
    payload = data[1:STATUS_INPUT_SIZE]
    if any(data[STATUS_INPUT_SIZE:]):
        raise ValueError("unexpected HID status report padding")
    version, sequence, uptime_ms, flags = struct.unpack("<BBIB", payload)
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID status report version {version}")
    if flags & ~STATUS_KNOWN_FLAGS:
        raise ValueError("unknown HID status flags")
    return {
        "version": version,
        "sequence": sequence,
        "uptime_ms": uptime_ms,
        "usb_mounted": bool(flags & STATUS_USB_MOUNTED),
        "cdc_open": bool(flags & STATUS_CDC_OPEN),
    }