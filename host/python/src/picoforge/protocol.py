"""Decoders for the PicoForge HID status protocol."""

from __future__ import annotations

import struct

INFO_REPORT_ID = 1
STATUS_REPORT_ID = 2
LED_COMMAND_REPORT_ID = 4
REPORT_VERSION = 2
INFO_PAYLOAD_SIZE = 42
STATUS_PAYLOAD_SIZE = 13
PRODUCT_NAME_SIZE = 20
SERIAL_NUMBER_SIZE = 16
INFO_FEATURE_SIZE = INFO_PAYLOAD_SIZE + 1
STATUS_INPUT_SIZE = STATUS_PAYLOAD_SIZE + 1

MCU_NAMES = {1: "RP2040", 2: "RP2350"}
STATUS_USB_MOUNTED = 1 << 0
STATUS_CDC_OPEN = 1 << 1
STATUS_TEMPERATURE_VALID = 1 << 2
STATUS_LED_AVAILABLE = 1 << 3
STATUS_LED_ON = 1 << 4
STATUS_KNOWN_FLAGS = (STATUS_USB_MOUNTED | STATUS_CDC_OPEN | STATUS_TEMPERATURE_VALID
                      | STATUS_LED_AVAILABLE | STATUS_LED_ON)


def _feature_payload(report: bytes | bytearray | list[int], report_id: int, size: int) -> bytes:
    data = bytes(report)
    if len(data) >= size + 2 and (
        data[0] == 0 and data[1] == report_id
    ):
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
    """Decode stable product, MCU, and USB serial identity from Feature report 1."""
    payload = _feature_payload(report, INFO_REPORT_ID, INFO_PAYLOAD_SIZE)
    version, mcu_id = payload[:2]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID info report version {version}")
    try:
        product_name = payload[2 : 2 + PRODUCT_NAME_SIZE].split(b"\0", 1)[0].decode("ascii")
        serial_number = payload[22 : 22 + SERIAL_NUMBER_SIZE].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("invalid text in HID identity report") from error
    if not product_name or len(serial_number) != SERIAL_NUMBER_SIZE or any(
        character not in "0123456789abcdefABCDEF" for character in serial_number
    ):
        raise ValueError("invalid product name or USB serial in HID identity report")
    if any(payload[38:]):
        raise ValueError("nonzero reserved bytes in HID identity report")
    return {
        "version": version,
        "mcu_id": mcu_id,
        "mcu": MCU_NAMES.get(mcu_id, f"Unknown MCU ({mcu_id})"),
        "product_name": product_name,
        "serial_number": serial_number,
    }


def parse_status_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode a report-ID-prefixed periodic HID Input report."""
    data = bytes(report)
    if len(data) < STATUS_INPUT_SIZE or data[0] != STATUS_REPORT_ID:
        raise ValueError("unexpected HID status report length or ID")
    payload = data[1:STATUS_INPUT_SIZE]
    if any(data[STATUS_INPUT_SIZE:]):
        raise ValueError("unexpected HID status report padding")
    version, sequence, uptime_ms, system_clock_hz, flags, temperature_centidegrees = struct.unpack(
        "<BBIIBh", payload
    )
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID status report version {version}")
    if system_clock_hz == 0:
        raise ValueError("invalid system clock in HID status report")
    if flags & ~STATUS_KNOWN_FLAGS:
        raise ValueError("unknown HID status flags")
    temperature_valid = bool(flags & STATUS_TEMPERATURE_VALID)
    led_available = bool(flags & STATUS_LED_AVAILABLE)
    led_on = bool(flags & STATUS_LED_ON)
    if led_on and not led_available:
        raise ValueError("LED state reported without an available LED")
    return {
        "version": version,
        "sequence": sequence,
        "uptime_ms": uptime_ms,
        "system_clock_hz": system_clock_hz,
        "usb_mounted": bool(flags & STATUS_USB_MOUNTED),
        "cdc_open": bool(flags & STATUS_CDC_OPEN),
        "temperature_valid": temperature_valid,
        "temperature_centidegrees": temperature_centidegrees if temperature_valid else None,
        "led_available": led_available,
        "led_on": led_on,
    }


def build_led_command_report(on: bool) -> bytes:
    """Build report-ID-prefixed HID Feature report 4 to set the board LED."""
    if not isinstance(on, bool):
        raise ValueError("LED state must be boolean")
    return bytes((LED_COMMAND_REPORT_ID, REPORT_VERSION, int(on)))