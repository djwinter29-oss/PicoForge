"""Compile the real logger against lightweight SDK stubs and exercise its public behavior."""

from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_logging_filter_format_truncation_and_output_result(tmp_path):
    compiler = shutil.which("gcc")
    if compiler is None:
        pytest.skip("gcc is required for the host-side logger runtime check")

    binary = tmp_path / "logging-runtime"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "tests/fixtures/logging-sdk"),
            "-I",
            str(ROOT / "firmware/src"),
            str(ROOT / "firmware/src/platform/logging.c"),
            str(ROOT / "tests/fixtures/logging-runtime.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

    too_small = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-DPF_LOG_LINE_BUFFER_SIZE=29u",
            "-I",
            str(ROOT / "tests/fixtures/logging-sdk"),
            "-I",
            str(ROOT / "firmware/src"),
            "-fsyntax-only",
            str(ROOT / "firmware/src/platform/logging.c"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert too_small.returncode != 0
    assert "Log line buffer must fit the longest error prefix and ellipsis" in too_small.stderr

    invalid_level_binary = tmp_path / "logging-invalid-level"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-DPF_TEST_LOGGING_DEFAULT_LEVEL=255u",
            "-I",
            str(ROOT / "tests/fixtures/logging-sdk"),
            "-I",
            str(ROOT / "firmware/src"),
            str(ROOT / "firmware/src/platform/logging.c"),
            str(ROOT / "tests/fixtures/logging-runtime.c"),
            "-o",
            str(invalid_level_binary),
        ],
        check=True,
    )
    invalid_level = subprocess.run(
        [str(invalid_level_binary)], capture_output=True, text=True, check=False
    )
    assert invalid_level.returncode != 0
    assert "Invalid logging default severity: 255" in invalid_level.stderr

    for define, error in (
        ("PF_TEST_LOGGING_UART_INDEX=2u", "Invalid logging UART index: 2"),
        ("PF_TEST_LOGGING_TX_GPIO=30u", "Invalid logging UART GPIO: tx 30, rx 5"),
        ("PF_TEST_LOGGING_BAUD_RATE=0u", "Invalid logging UART baud rate: 0"),
    ):
        invalid_config_binary = tmp_path / define.split("=")[0]
        subprocess.run(
            [
                compiler,
                "-std=c11",
                f"-D{define}",
                "-I",
                str(ROOT / "tests/fixtures/logging-sdk"),
                "-I",
                str(ROOT / "firmware/src"),
                str(ROOT / "firmware/src/platform/logging.c"),
                str(ROOT / "tests/fixtures/logging-runtime.c"),
                "-o",
                str(invalid_config_binary),
            ],
            check=True,
        )
        invalid_config = subprocess.run(
            [str(invalid_config_binary)], capture_output=True, text=True, check=False
        )
        assert invalid_config.returncode != 0
        assert error in invalid_config.stderr

    mutex_disabled = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-DPICO_STDOUT_MUTEX=0",
            "-I",
            str(ROOT / "tests/fixtures/logging-sdk"),
            "-I",
            str(ROOT / "firmware/src"),
            "-fsyntax-only",
            str(ROOT / "firmware/src/platform/logging.c"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert mutex_disabled.returncode != 0
    assert "requires Pico SDK stdout serialization" in mutex_disabled.stderr
