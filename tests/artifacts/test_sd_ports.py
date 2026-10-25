#!/usr/bin/env python3
"""Test bare-metal cross-build artifacts; C behavior is covered by Ceedling."""
import json
import os
from pathlib import Path
import re
import subprocess

import pytest

pytestmark = pytest.mark.artifacts

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "firmware/src/drivers/sdcard"


def check_build(directory):
    directory = directory.resolve()
    commands = json.loads((directory / "compile_commands.json").read_text())
    files = [item["file"] for item in commands]
    ports = [file for file in files if Path(file).parent == DRIVER and Path(file).name.startswith("sd_port_")]
    expected = str(DRIVER / "sd_port_baremetal.c")
    if ports != [expected]:
        raise AssertionError(f"Expected only bare-metal port in {directory}: {ports}")
    for item in commands:
        if "FreeRTOS" in item["command"] or "FreeRTOS" in item["file"]:
            raise AssertionError(f"Kernel dependency in {directory}: {item['file']}")
    elf = directory / "pf_sdcard_baremetal_smoke.elf"
    symbols = subprocess.check_output(
        [os.environ.get("ARM_NM", "arm-none-eabi-nm"), "-S", str(elf)], text=True)
    if re.search(r"\b(?:[xv]Task\w*|[xv]Queue\w*|[xv]Port\w*|pvPort\w*|vApplication\w*)$", symbols,
                 re.MULTILINE):
        raise AssertionError(f"FreeRTOS symbols in {elf}")
    for symbol in ("pf_sd_init", "pf_sd_read_blocks", "pf_sd_write_blocks",
                   "pf_sd_sync", "pf_sd_port_try_lock"):
        if not re.search(r"\b" + symbol + r"$", symbols, re.MULTILINE):
            raise AssertionError(f"Missing {symbol} in {elf}")
    if re.search(r"\bpf_sd_spi_read_blocks$", symbols, re.MULTILINE):
        native_symbols = set(re.findall(r"\b(pf_sd_native_\w+|crc16_4bit_\w+)$", symbols, re.MULTILINE))
        shared_switch_decoders = {
            "pf_sd_native_switch_supports_high_speed",
            "pf_sd_native_switch_selected_high_speed",
            "pf_sd_native_switch_high_speed_busy",
        }
        if native_symbols - shared_switch_decoders:
            raise AssertionError(f"Unused native wire code or CRC tables in SPI artifact: {elf}")
    else:
        protocol = [item for item in commands if item["file"].endswith("/sdcard/sd_protocol.c")]
        cache = (directory / "CMakeCache.txt").read_text()
        fused = re.search(r"^PF_SDIO_PIO_FUSED_COPY:BOOL=(.+)$", cache, re.MULTILINE)
        if fused is None:
            raise AssertionError(f"Missing native fused-copy configuration in {directory}")
        enabled = fused[1].upper() in ("1", "ON", "TRUE", "YES", "Y")
        if len(protocol) != 1 or f"-DPF_SD_NATIVE_FUSED_COPY={int(enabled)}" not in protocol[0]["command"]:
            raise AssertionError(f"Fused-copy option did not reach the protocol implementation in {directory}")
        for table in ("crc16_4bit_slice_hi", "crc16_4bit_slice_lo"):
            if not re.search(rf"^.*\s00001000\s[bB]\s{table}$", symbols, re.MULTILINE):
                raise AssertionError(f"Missing 4 KiB native CRC table {table} in {elf}")
    print(f"Bare-metal compile/link: exactly one port, no kernel includes/symbols: {elf}")


def test_baremetal_port(baremetal_build_dir):
    if baremetal_build_dir is None:
        pytest.fail("Port artifact tests require --baremetal-build-dir PATH (build bare-metal firmware first)")
    check_build(baremetal_build_dir)
