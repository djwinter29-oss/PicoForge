"""Model assembled NOR symbol instructions; not electrical qualification."""
import os
from pathlib import Path
import re
import subprocess

import pytest

pytestmark = pytest.mark.pio
ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def programs(tmp_path_factory):
    candidates = sorted(ROOT.glob("build/*/pioasm/pioasm"))
    assembler = os.environ.get("PIOASM") or (str(candidates[0]) if candidates else None)
    assert assembler, "Build firmware first or set PIOASM"
    output = tmp_path_factory.mktemp("nor-pio") / "nor.h"
    subprocess.run([assembler, "-o", "c-sdk",
                    str(ROOT / "firmware/src/drivers/storage/qspi_pio/storage_qspi_pio.pio"),
                    str(output)], check=True)
    text = output.read_text()
    return {
        name: [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]{4})",
               text.split(f"pf_storage_{name}_program_instructions[] = {{", 1)[1].split("};", 1)[0])]
        for name in ("single", "quad")
    }


@pytest.mark.parametrize("lanes,name", [(1, "single"), (4, "quad")])
@pytest.mark.parametrize("value", [0x00, 0x01, 0x9f, 0xa5, 0xff])
def test_symbol_order_and_clock_edges(programs, lanes, name, value):
    ops = programs[name]
    assert len(ops) == 4
    assert [op >> 13 for op in ops] == [4, 3, 2, 4]  # PULL, OUT, IN, PUSH
    assert [bool(op & 0x1000) for op in ops] == [False, False, True, False]
    assert ops[0] & 0xff == 0xa0  # blocking pull
    assert ops[3] & 0xff == 0x20  # blocking push
    assert ops[1] & 0xff == lanes  # OUT PINS
    assert ops[2] & 0xff == lanes  # IN PINS
    symbols = [(value >> shift) & ((1 << lanes) - 1)
               for shift in range(8 - lanes, -1, -lanes)]
    received = 0
    for symbol in symbols:
        osr = symbol << (32 - lanes)
        pins = osr >> (32 - lanes)
        received = (received << lanes) | pins
    assert received == value
    assert len(symbols) == 8 // lanes


def test_both_programs_fit_pio_memory(programs):
    assert sum(len(ops) for ops in programs.values()) <= 32
