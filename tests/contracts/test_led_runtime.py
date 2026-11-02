"""Exercise build-selected LED writers, polarity, readiness, and GPIO validation without hardware."""

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
    ids=["active-high", "active-low", "wireless-led", "invalid-gpio"],
)
def test_led_polarity_writer_readiness_and_gpio_validation(tmp_path, defines):
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


@pytest.mark.parametrize("wireless", [False, True], ids=["gpio-board", "wireless-board"])
def test_board_led_assignment_initializes(tmp_path, wireless):
    """W boards have no LED GPIO, so the real board.c must attach the CYW43 writer or boot panics before USB."""
    compiler = shutil.which("gcc")
    if compiler is None:
        pytest.skip("gcc is required for the host-side board LED check")
    (tmp_path / "pico").mkdir()
    (tmp_path / "pico/stdlib.h").write_text("" if wireless else "#define PICO_DEFAULT_LED_PIN 25\n")
    (tmp_path / "harness.c").write_text(r"""
#include <assert.h>
#include <stddef.h>
#include "hardware/gpio.h"
#include "platform/led.h"
#include "app/wireless/wireless.h"
void gpio_init(uint gpio) { (void)gpio; }
void gpio_set_dir(uint gpio, bool out) { (void)gpio; (void)out; }
void gpio_put(uint gpio, bool value) { (void)gpio; (void)value; }
bool pf_wireless_led_put(bool on) { (void)on; return true; }
int main(void)
{
    assert(pf_led_init());
#if PF_ENABLE_WIRELESS
    assert(pf_led_board.put == pf_wireless_led_put);
#else
    assert(pf_led_board.put == NULL && pf_led_board.gpio == 25u);
#endif
    return 0;
}
""")
    binary = tmp_path / "board-led"
    subprocess.run(
        [
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", *(["-DPF_ENABLE_WIRELESS=1"] if wireless else []),
            "-I", str(tmp_path), "-I", str(ROOT / "tests/fixtures/led-sdk"), "-I", str(ROOT / "firmware/src"),
            str(ROOT / "firmware/src/config/board.c"), str(ROOT / "firmware/src/platform/led.c"),
            str(tmp_path / "harness.c"), "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
