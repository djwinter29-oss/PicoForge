"""Exercise production radio retries, raw SSIDs and scan generations without hardware."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_lwip_heap_is_word_aligned():
    options = (ROOT / "firmware/src/integration/wireless/lwipopts.h").read_text()
    assert "#define MEM_ALIGNMENT 4" in options


def test_wifi_boot_gating_deadline_retries_and_ipv4(tmp_path):
    binary = tmp_path / "wifi"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "firmware/tests/support/wireless"),
        "-I", str(ROOT / "firmware/tests/support/lwip"),
        "-I", str(ROOT / "firmware/src"), "-x", "c",
        str(ROOT / "firmware/tests/support/wireless/lifecycle.inc"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary), "wifi"], check=True)
