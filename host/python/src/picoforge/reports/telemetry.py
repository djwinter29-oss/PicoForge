"""Telemetry HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .models import RuntimeStatus, SupportedHardwareIdentity

from .common import (
    REPORT_VERSION,
    TELEMETRY_CDC_OPEN,
    TELEMETRY_LED_AVAILABLE,
    TELEMETRY_PAYLOAD_SIZE,
    TELEMETRY_REPORT_ID,
    TELEMETRY_TEMPERATURE_VALID,
    TELEMETRY_USB_MOUNTED,
    _input_payload,
)


def parse_status_report(
    report: bytes | bytearray | list[int], identity: SupportedHardwareIdentity | None = None
) -> RuntimeStatus:
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
