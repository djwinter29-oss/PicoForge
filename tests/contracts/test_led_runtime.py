"""Compile the real LED driver against lightweight GPIO stubs and exercise polarity and validation."""

from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize(
    "defines",
    [
        [],
        ["-DPF_TEST_LED_ACTIVE_HIGH=false"],
        ["-DPF_TEST_LED_GPIO=255u"],
        ["-DPF_TEST_LED_GPIO=30u"],
    ],
    ids=["active-high", "active-low", "no-led", "invalid-gpio"],
)
def test_led_polarity_availability_and_gpio_validation(tmp_path, defines):
    compiler = shutil.which("gcc")
    if compiler is None:
        pytest.skip("gcc is required for the host-side LED runtime check")

    binary = tmp_path / "led-runtime"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *defines,
            "-I",
            str(ROOT / "tests/fixtures/led-sdk"),
            "-I",
            str(ROOT / "firmware/src"),
            str(ROOT / "firmware/src/platform/led.c"),
            str(ROOT / "tests/fixtures/led-runtime.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
