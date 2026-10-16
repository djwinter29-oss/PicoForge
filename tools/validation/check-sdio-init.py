#!/usr/bin/env python3
"""Static check for the qualified board SDIO clock request."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD_PROFILE = ROOT / "firmware/src/board/board_profile.c"


def main():
    profile = BOARD_PROFILE.read_text()
    assert ".clock_hz = 25000000u," in profile, (
        "The qualified stock Pico 2 SDIO profile must request 25 MHz")

    system_clock_hz = 150000000
    requested_clock_hz = 25000000
    divider = (system_clock_hz + 2 * requested_clock_hz - 1) // (2 * requested_clock_hz)
    assert divider == 3
    assert system_clock_hz // (2 * divider) == requested_clock_hz
    print("SDIO board clock static check passed")


if __name__ == "__main__":
    main()
