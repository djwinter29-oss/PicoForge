# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Exercise Report 12 and typed staging lifecycle against the production C modules."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_wireless_stack_credentials_use_non_elidable_cleanup():
    source = (ROOT / "firmware/src/app/wireless/wireless.c").read_text()
    assert "mbedtls_platform_zeroize(key, sizeof(key))" in source
    assert "mbedtls_platform_zeroize(password, sizeof(password))" in source
    assert "mbedtls_platform_zeroize(&staged, sizeof(staged))" in source


def test_wireless_staging_expiry_pair_safety_and_key_gated_services(tmp_path):
    binary = tmp_path / "wireless"
    compiled = subprocess.run([
        "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "firmware/tests/support/wireless"),
        "-I", str(ROOT / "firmware/tests/support/lwip"),
        "-I", str(ROOT / "firmware/src"), "-x", "c",
        str(ROOT / "firmware/tests/support/wireless/lifecycle.inc"), "-o", str(binary),
    ], check=False, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    subprocess.run([str(binary), "provisioning"], check=True)
