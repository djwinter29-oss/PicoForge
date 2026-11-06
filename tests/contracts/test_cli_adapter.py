# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Require an explicit adapter review whenever its upstream editor implementation changes."""
import hashlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize("filename,digest", [
    ("src/embedded_cli.c", "c44041a7c0d07226143001c4440c79923bafb2d33ab12641fc7737bbb42990bc"),
    ("include/embedded_cli.h", "63c499cd0f7c7095a032a266538ffa69ebd9b4738a90af1bee55c39b2c660daf"),
])
def test_cli_adapter_upstream_has_been_reviewed(filename, digest):
    source = ROOT / "firmware/third_party/embedded-cli/lib" / filename
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, (
        "Review embedded_cli_adapter.c flags, editor/history/escape handling and RX overflow semantics; "
        "run the CLI regression suites before updating this reviewed source digest."
    )
