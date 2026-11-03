# Demonstration Records

This document records interface observations within the project scope.
The [README](../../../README.md) shows a clearly labeled simulated Direct dashboard preview; the earlier Python
host observation is documented below. Demonstrations are not hardware qualification.

## Simulated Direct Dashboard Preview

The [full dashboard view](../../assets/dashboard-direct-demo.png) was captured on 2026-11-03 from the current Direct
dashboard using mocked HID reports. It renders an illustrative Pico 2 W / RP2350 with Wi-Fi and Bluetooth enabled
and connected, and SDIO storage reporting a 60 GiB exFAT SanDisk card. The displayed manufacturer, product, serial,
capacity, and ready state are sample values. No physical board, SD card, Wi-Fi network, or Bluetooth device was
connected. Every displayed reading and connected state is synthetic; this preview does not demonstrate firmware,
USB, SD-card, wireless, or benchmark behavior and is not hardware evidence.

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
