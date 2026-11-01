# Demonstration Records

This document records observed interface behavior within the [project scope](../../../README.md#project-scope).
The connected screenshot is included in the [README](../../../README.md).
Demonstrations are not hardware qualification.

## Captured Dashboard

[Python dashboard screenshot](../../assets/dashboard-python.jpg), captured on 2026-10-26 from the running Python host:

- Pico 2 (RP2350), UID `575393D4D2C8C043`, system clock 150 MHz.
- Reported firmware version `0.0.0`; the running firmware source revision was not established by this capture.
- SDIO at 37.5 MHz, High-Speed; 119.08 GiB exFAT SanDisk SB128 card.
- Python dashboard accessed through the shared browser at `http://127.0.0.1:5001/`.
- The page reported a connected board and live HID telemetry. No settings, ownership, or card contents were changed.
- Self-test results were already displayed, not generated or independently verified during capture.

This image documents the visible interface and reported state, not performance or hardware qualification.

## Evidence Requirements

- Use real screenshots and observed behavior, not mock results presented as hardware evidence.
- Runnable examples must use implemented APIs and identify required hardware, wiring, dependencies, and commands.
- State the firmware revision, board, host environment, and relevant peripheral or card configuration.
- Distinguish a successful walkthrough from broader reliability or compatibility qualification.

Detailed measurements and reviews belong in the [test records](README.md).
Outstanding reliability and compatibility limits remain in [qualification gaps](../qualification-gaps.md).
Screenshots and demonstrations do not close those gaps.
