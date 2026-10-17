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
DRIVER = ROOT / "firmware/src/drivers/sd_card"


def check_build(directory):
    directory = directory.resolve()
    commands = json.loads((directory / "compile_commands.json").read_text())
    files = [item["file"] for item in commands]
    ports = [file for file in files if "/drivers/sd_card/ports/" in file]
    expected = str(DRIVER / "ports/baremetal/sd_port.c")
    if ports != [expected]:
        raise AssertionError(f"Expected only bare-metal port in {directory}: {ports}")
    for item in commands:
        if "FreeRTOS" in item["command"] or "FreeRTOS" in item["file"]:
            raise AssertionError(f"Kernel dependency in {directory}: {item['file']}")
    elf = directory / "pf_sd_card_baremetal_smoke.elf"
    symbols = subprocess.check_output(
        [os.environ.get("ARM_NM", "arm-none-eabi-nm"), str(elf)], text=True)
    if re.search(r"\b(?:[xv]Task\w*|[xv]Queue\w*|[xv]Port\w*|pvPort\w*|vApplication\w*)$", symbols,
                 re.MULTILINE):
        raise AssertionError(f"FreeRTOS symbols in {elf}")
    for symbol in ("pf_sd_init", "pf_sd_read_blocks", "pf_sd_write_blocks",
                   "pf_sd_sync", "pf_sd_port_try_lock"):
        if not re.search(r"\b" + symbol + r"$", symbols, re.MULTILINE):
            raise AssertionError(f"Missing {symbol} in {elf}")
    print(f"Bare-metal compile/link: exactly one port, no kernel includes/symbols: {elf}")


def test_baremetal_port(baremetal_build_dir):
    if baremetal_build_dir is None:
        pytest.fail("Port artifact tests require --baremetal-build-dir PATH (build bare-metal firmware first)")
    check_build(baremetal_build_dir)
