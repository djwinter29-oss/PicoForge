# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Identity HID report codecs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .models import BoardConfiguration, HardwareIdentity, SupportedHardwareIdentity

from .common import (
    BOARD_NAMES,
    CAP_KNOWN,
    CAP_LED,
    CAP_REBOOT,
    CAP_SAVE_SETTINGS,
    CAP_SDIO,
    CAP_SELF_TEST,
    CAP_SPI,
    CAP_WIRELESS,
    CONFIGURATION_PAYLOAD_SIZE,
    CONFIGURATION_REPORT_ID,
    IDENTITY_PAYLOAD_SIZE,
    IDENTITY_REPORT_ID,
    MCU_NAMES,
    PRODUCT_NAMES,
    REPORT_VERSION,
)
from .common import _feature_payload, _unknown_name


def parse_identity_report(report: bytes | bytearray | list[int]) -> HardwareIdentity:
    """Decode and validate Feature report 1."""
    payload = _feature_payload(report, IDENTITY_REPORT_ID, IDENTITY_PAYLOAD_SIZE)
    version = payload[0]
    product_id = int.from_bytes(payload[1:3], "little")
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID identity report version {version}")
    if product_id != 2:
        return {
            "version": version,
            "product_id": product_id,
            "product_name": PRODUCT_NAMES.get(product_id, f"Unknown product (0x{product_id:04X})"),
            "supported": False,
            "capabilities": 0,
            "led_available": False,
            "reboot_available": False,
            "settings_supported": False,
            "self_test_supported": False,
            "wireless_supported": False,
        }
    uid = payload[3:11]
    firmware = tuple(payload[11:14])
    mcu_id, board_model, _, capabilities = payload[14:18]
    ram_total, ram_reserved = struct.unpack_from("<II", payload, 18)
    if capabilities & ~CAP_KNOWN or bool(capabilities & CAP_SPI) == bool(capabilities & CAP_SDIO):
        raise ValueError("invalid HID identity capability flags or storage transport")
    if board_model in (1, 4) and mcu_id != 1 or board_model in (2, 3) and mcu_id != 2:
        raise ValueError("board model and MCU disagree in HID identity report")
    if not ram_total or ram_reserved > ram_total:
        raise ValueError("invalid SRAM accounting in HID identity report")
    return {
        "version": version,
        "product_id": product_id,
        "product_name": PRODUCT_NAMES[product_id],
        "supported": True,
        "device_uid": uid.hex().upper(),
        "serial_number": uid.hex().upper(),
        "firmware_major": firmware[0],
        "firmware_minor": firmware[1],
        "firmware_patch": firmware[2],
        "firmware_version": ".".join(map(str, firmware)),
        "mcu_id": mcu_id,
        "mcu": _unknown_name(mcu_id, MCU_NAMES),
        "board_model_id": board_model,
        "board_model": _unknown_name(board_model, BOARD_NAMES),
        "capabilities": capabilities,
        "led_available": bool(capabilities & CAP_LED),
        "reboot_available": bool(capabilities & CAP_REBOOT),
        "settings_supported": bool(capabilities & CAP_SAVE_SETTINGS),
        "storage_transport": "spi" if capabilities & CAP_SPI else "sdio",
        "self_test_supported": bool(capabilities & CAP_SELF_TEST),
        "wireless_supported": bool(capabilities & CAP_WIRELESS),
        "ram_total_bytes": ram_total,
        "ram_reserved_bytes": ram_reserved,
    }

def parse_configuration_report(
    report: bytes | bytearray | list[int], identity: SupportedHardwareIdentity
) -> BoardConfiguration:
    """Decode Feature report 2 and validate transport- and board-dependent fields."""
    payload = _feature_payload(report, CONFIGURATION_REPORT_ID, CONFIGURATION_PAYLOAD_SIZE)
    version = payload[0]
    clock_profile = int.from_bytes(payload[1:3], "little")
    sd_profile = payload[3]
    uart_index, uart_tx, uart_rx = payload[4:7]
    storage_gpios = tuple(payload[7:13])
    flags, reset_cause, led_gpio, card_detect_gpio = payload[13:17]
    if version != REPORT_VERSION:
        raise ValueError(f"unsupported HID board configuration report version {version}")
    if clock_profile not in (0, 150, 200, 250) or sd_profile not in (0, 1):
        raise ValueError("invalid stored board configuration profile")
    if flags & ~0x3F:
        raise ValueError("unknown board configuration flags")
    radios = flags >> 3 & 3
    if flags & 0x38 and not identity.get("wireless_supported"):
        raise ValueError("radio enables reported without wireless capability")
    if uart_index not in (0, 1, 255):
        raise ValueError("invalid monitor UART index")
    if (uart_tx == 255) != (uart_rx == 255):
        raise ValueError("monitor UART pins must both be assigned or unavailable")
    if (uart_index == 255) != (uart_tx == 255):
        raise ValueError("monitor UART index and GPIO availability disagree")
    if flags & 0x04 and (uart_index == 255 or uart_tx == 255):
        raise ValueError("enabled monitor UART requires an index and both GPIOs")
    if bool(flags & 0x01) != (card_detect_gpio != 255):
        raise ValueError("card-detect flag and GPIO disagree")
    if bool(flags & 0x02) != (led_gpio != 255):
        raise ValueError("LED flag and GPIO disagree")
    if flags & 0x02 and not identity["led_available"]:
        raise ValueError("identity and configuration LED availability disagree")
    if flags & 0x04 and uart_index == 255:
        raise ValueError("enabled monitor UART is unavailable")
    transport = identity["storage_transport"]
    if transport == "sdio":
        if any(gpio == 255 for gpio in storage_gpios):
            raise ValueError("SDIO configuration requires six storage GPIOs")
    elif any(gpio == 255 for gpio in storage_gpios[:4]) or any(
        gpio != 255 for gpio in storage_gpios[4:]
    ):
        raise ValueError("invalid SPI storage GPIO layout")
    board_model = identity["board_model_id"]
    mcu_id = identity["mcu_id"]
    if mcu_id in (1, 2):
        max_gpio = 29 if mcu_id == 1 or board_model in (1, 2, 3, 4) else 47
        assigned = [gpio for gpio in (uart_tx, uart_rx, led_gpio, card_detect_gpio, *storage_gpios)
                    if gpio != 255]
        if any(gpio > max_gpio for gpio in assigned):
            raise ValueError("assigned GPIO is invalid for the reported MCU and board")
    return {
        "version": version,
        "stored_sysclk_profile": clock_profile,
        "system_clock_mhz": clock_profile,
        "stored_sdcard_profile": sd_profile,
        "sdcard_profile": sd_profile,
        "radios": radios,
        "tcp_cli_enabled": bool(flags & 0x20),
        "monitor_uart_index": uart_index,
        "monitor_uart_tx_gpio": uart_tx,
        "monitor_uart_rx_gpio": uart_rx,
        "storage_gpios": storage_gpios,
        "board_flags": flags,
        "reset_cause": reset_cause,
        "led_gpio": led_gpio,
        "card_detect_gpio": card_detect_gpio,
        "storage_transport": transport,
    }
