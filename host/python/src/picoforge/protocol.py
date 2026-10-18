"""Decoders for the PicoForge HID status protocol."""

from __future__ import annotations

import struct

INFO_REPORT_ID = 1
STATUS_REPORT_ID = 2
MEMORY_REPORT_ID = 3
LED_COMMAND_REPORT_ID = 4
MSC_COMMAND_REPORT_ID = 5
SELF_TEST_REPORT_ID = 6
FILESYSTEM_REPORT_ID = 7
SELF_TEST_PAYLOAD_SIZE = 32
SELF_TEST_INPUT_SIZE = SELF_TEST_PAYLOAD_SIZE + 1
FILESYSTEM_PAYLOAD_SIZE = 3
FILESYSTEM_INPUT_SIZE = FILESYSTEM_PAYLOAD_SIZE + 1
REPORT_VERSION = 2
INFO_PAYLOAD_SIZE = 42
STATUS_PAYLOAD_SIZE = 23
MEMORY_PAYLOAD_SIZE = 29
PRODUCT_NAME_SIZE = 20
SERIAL_NUMBER_SIZE = 16
INFO_FEATURE_SIZE = INFO_PAYLOAD_SIZE + 1
STATUS_INPUT_SIZE = STATUS_PAYLOAD_SIZE + 1
MEMORY_INPUT_SIZE = MEMORY_PAYLOAD_SIZE + 1

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
    if version not in (2, 3):
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
    if any(payload[38:] if version == 2 else payload[41:]):
        raise ValueError("nonzero reserved bytes in HID identity report")
    return {
        "version": version,
        "mcu_id": mcu_id,
        "mcu": MCU_NAMES.get(mcu_id, f"Unknown MCU ({mcu_id})"),
        "product_name": product_name,
        "serial_number": serial_number,
        "firmware_version": ".".join(str(value) for value in payload[38:41]) if version == 3 else None,
    }


def parse_status_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode a report-ID-prefixed periodic HID Input report."""
    data = bytes(report)
    if len(data) < 14 or data[0] != STATUS_REPORT_ID:
        raise ValueError("unexpected HID status report length or ID")
    version = data[1]
    if version not in (2, 3):
        raise ValueError(f"unsupported HID status report version {version}")
    size = STATUS_INPUT_SIZE if version == 3 else 14
    if len(data) < size:
        raise ValueError("unexpected HID status report length")
    payload = data[1:size]
    if any(data[size:]):
        raise ValueError("unexpected HID status report padding")
    version, sequence, uptime_ms, system_clock_hz, flags, temperature_centidegrees = struct.unpack(
        "<BBIIBh", payload[:13]
    )
    cpu_usage = ram_used = ram_total = None
    if version == 3:
        cpu_usage, ram_used, ram_total = struct.unpack("<HII", payload[13:])
        if cpu_usage > 10000 and cpu_usage != 0xffff:
            raise ValueError("invalid CPU usage in HID status report")
        if ram_total == 0 or ram_used > ram_total:
            raise ValueError("invalid RAM usage in HID status report")
        cpu_usage = None if cpu_usage == 0xffff else cpu_usage
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
        "cpu_usage_basis_points": cpu_usage,
        "ram_used_bytes": ram_used,
        "ram_total_bytes": ram_total,
    }


def parse_memory_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode dynamic SD-card state, negotiated link, capacity, and CID."""
    data = bytes(report)
    if len(data) < MEMORY_INPUT_SIZE or data[0] != MEMORY_REPORT_ID:
        raise ValueError("unexpected HID memory report length or ID")
    payload = data[1:MEMORY_INPUT_SIZE]
    if any(data[MEMORY_INPUT_SIZE:]):
        raise ValueError("unexpected HID memory report padding")
    version, sequence, flags, component_type, mode, clock_hz, blocks = struct.unpack(
        "<BBBBBII", payload[:13]
    )
    cid = payload[13:29]
    nor = version == 3 and component_type == 2
    if version != REPORT_VERSION and not nor:
        raise ValueError(f"unsupported HID memory report version {version}")
    if flags & ~0x3f:
        raise ValueError("unknown HID memory flags")
    valid_mode = mode in (2, 3) if nor else component_type in (0, 1) and mode in (0, 1, 2)
    if not valid_mode:
        raise ValueError("unknown HID memory type or mode")
    configured, presence_known, present, initialized, cid_valid, capacity_valid = (
        bool(flags & (1 << bit)) for bit in range(6)
    )
    if present and not presence_known:
        raise ValueError("memory reported present without known presence")
    if initialized and (not configured or not present or not presence_known):
        raise ValueError("initialized memory requires a configured, present card")
    if cid_valid != initialized or capacity_valid != initialized:
        raise ValueError("initialized memory metadata flags disagree")
    if initialized != bool(clock_hz and blocks) or (not initialized and any(cid)):
        raise ValueError("invalid memory metadata values")
    manufacturer = None
    if nor and initialized:
        if (cid[:3] != bytes.fromhex("ef4018") or any(cid[7:])
            or int.from_bytes(cid[3:7], "little") != 16777216 or blocks * 512 > 16777216):
            raise ValueError("invalid NOR identification or capacity")
        manufacturer = {"id": cid[0], "product_name": "W25Q128", "jedec_id": "ef4018",
                        "serial_number": None}
    elif cid_valid:
        try:
            oem = cid[1:3].decode("ascii")
            product = cid[3:8].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("invalid text in SD CID") from error
        revision = cid[8]
        manufacturing_date = ((cid[13] & 0x0f) << 8) | cid[14]
        manufacturer = {
            "id": cid[0],
            "oem_id": oem,
            "product_name": product,
            "product_revision": f"{revision >> 4}.{revision & 0x0f}",
            "serial_number": int.from_bytes(cid[9:13], "big"),
            "manufacture_year": 2000 + (manufacturing_date >> 4),
            "manufacture_month": manufacturing_date & 0x0f,
        }
    return {
        "version": version,
        "sequence": sequence,
        "configured": configured,
        "presence_known": presence_known,
        "present": present if presence_known else None,
        "initialized": initialized,
        "component_type": "nor-flash" if nor else "sd-card" if component_type == 1 else None,
        "mode": {0: None, 1: "sdio", 2: "spi", 3: "qspi"}[mode],
        "bus_clock_hz": clock_hz if initialized else None,
        "block_count": blocks if capacity_valid else None,
        "capacity_bytes": blocks * 512 if capacity_valid else None,
        "manufacturer": manufacturer,
        **({"physical_capacity_bytes": int.from_bytes(cid[3:7], "little") if initialized else None} if nor else {}),
    }


def parse_filesystem_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    """Decode the last successfully mounted filesystem type from Input report 7."""
    data = bytes(report)
    if len(data) < FILESYSTEM_INPUT_SIZE or data[0] != FILESYSTEM_REPORT_ID:
        raise ValueError("unexpected HID filesystem report length or ID")
    payload = data[1:FILESYSTEM_INPUT_SIZE]
    if any(data[FILESYSTEM_INPUT_SIZE:]):
        raise ValueError("unexpected HID filesystem report padding")
    version, sequence, format_id = payload
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID filesystem report version {version}")
    formats = {0: "unknown", 1: "fat12", 2: "fat16", 3: "fat32", 4: "exfat"}
    if format_id not in formats:
        raise ValueError("unknown HID filesystem format")
    return {"version": version, "sequence": sequence, "format": formats[format_id]}


def build_led_command_report(on: bool) -> bytes:
    """Build report-ID-prefixed HID Feature report 4 to set the board LED."""
    if not isinstance(on, bool):
        raise ValueError("LED state must be boolean")
    return bytes((LED_COMMAND_REPORT_ID, REPORT_VERSION, int(on)))


def parse_msc_status_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    data = bytes(report)
    offset = 1 if len(data) >= 5 and data[0] == MSC_COMMAND_REPORT_ID else 0
    payload = data[offset : offset + 4]
    if len(payload) != 4 or any(data[offset + 4 :]):
        raise ValueError("unexpected HID MSC status report length or padding")
    version, enabled, pending, rejected = payload
    if version not in (REPORT_VERSION, 3) or any(value > 1 for value in (enabled, pending, rejected)):
        raise ValueError("invalid HID MSC status report")
    return {"enabled": bool(enabled), "transition_pending": bool(pending),
            "request_rejected": bool(rejected), "force_return_supported": version == 3}


def build_msc_command_report(enable: bool, *, force: bool = False) -> bytes:
    if not isinstance(enable, bool):
        raise ValueError("MSC enable state must be boolean")
    if not isinstance(force, bool) or (force and enable):
        raise ValueError("Force return requires disabling MSC")
    if force:
        return bytes((MSC_COMMAND_REPORT_ID, 3, 2))
    return bytes((MSC_COMMAND_REPORT_ID, REPORT_VERSION, int(enable)))


def build_self_test_command_report(action: str, size_mib: int = 32) -> bytes:
    if action not in ("start", "cancel"):
        raise ValueError("Self-test action must be start or cancel")
    if action == "cancel":
        size_mib = 0
    elif not isinstance(size_mib, int) or isinstance(size_mib, bool) or not 1 <= size_mib <= 1024:
        raise ValueError("Self-test size must be an integer from 1 to 1024 MiB")
    return bytes((SELF_TEST_REPORT_ID, REPORT_VERSION, int(action == "start"))) + struct.pack("<I", size_mib)


def parse_self_test_report(report: bytes | bytearray | list[int]) -> dict[str, object]:
    payload = _feature_payload(report, SELF_TEST_REPORT_ID, SELF_TEST_PAYLOAD_SIZE)
    version, sequence, state, flags, target, written, read, write_ms, read_ms, mismatches, error, fs_result, request_error, reserved = struct.unpack(
        "<BBBBIIIIIIBBBB", payload
    )
    if (version != REPORT_VERSION or state > 8 or flags & ~3 or reserved
        or error > 11 or request_error > 11 or fs_result > 19
        or target > 1024 * 1024 * 1024 or written > target or read > written
        or (state == 6 and (not target or written != target or read != target or mismatches or error or flags & 1))):
        raise ValueError("Invalid HID self-test report")
    names = ("idle", "queued", "mounting", "writing", "reading", "cleanup", "passed", "failed", "cancelled")
    return {
        "version": version, "sequence": sequence, "state": names[state],
        "active": 1 <= state <= 5, "target_bytes": target,
        "written_bytes": written, "read_bytes": read,
        "write_elapsed_ms": write_ms, "read_elapsed_ms": read_ms,
        "write_bytes_per_second": written * 1000 / write_ms if write_ms else None,
        "read_bytes_per_second": read * 1000 / read_ms if read_ms else None,
        "progress_percent": (written + read) * 100 / (2 * target) if target else 0,
        "mismatch_count": mismatches, "error": error, "filesystem_result": fs_result,
        "request_error": request_error, "file_remains": bool(flags & 1),
        "volume_mounted": bool(flags & 2),
    }