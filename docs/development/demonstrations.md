# Demonstration Plan

This document tracks demonstrations for the user and developer experience described in the
[project vision](../design/vision.md). The connected screenshot is included in the [README](../../README.md);
the remaining demonstrations below are still to add.
This is not a feature delivery schedule or a record of completed validation.

## Captured Dashboard

[Python dashboard screenshot](../images/dashboard-python.jpg), captured on 2026-10-26 from the running Python host:

- Pico 2 (RP2350), UID `575393D4D2C8C043`, system clock 150 MHz.
- Reported firmware version `0.0.0`; the running firmware source revision was not established by this capture.
- SDIO at 37.5 MHz, High-Speed; 119.08 GiB exFAT SanDisk SB128 card.
- Python dashboard accessed through the shared browser at `http://127.0.0.1:5001/`.
- The page reported a connected board and live HID telemetry. No settings, ownership, or card contents were changed.
- Self-test results were already displayed, not generated or independently verified during capture.

This image documents the visible interface and reported state, not performance or hardware qualification.

## Demonstrations Still to Add

| Evidence to add | What it should demonstrate | Prerequisite |
| --- | --- | --- |
| **First-use walkthrough** | Flash firmware, connect a host, inspect the board, and change a supported setting. | Record the current workflow and relevant setup dependencies. |
| **Storage workflow demonstration** | File access and ownership handoff, including safe eject, return, and visible failure outcomes. | Use backed-up or expendable media and document the host/card configuration. |
| **Sensor-to-host example** | Read a documented peripheral from Python and C#, with wiring and transaction errors explained. | Implement the peripheral bridge and supported host APIs first. |
| **Developer extension example** | A supported custom build connecting an application-specific capability through firmware, protocol, host code, and tests. | Provide a runnable extension and document its supported build configuration. |

## Evidence Requirements

- Use real screenshots and observed behavior, not mock results presented as hardware evidence.
- Runnable examples must use implemented APIs and identify required hardware, wiring, dependencies, and commands.
- State the firmware revision, board, host environment, and relevant peripheral or card configuration.
- Distinguish a successful walkthrough from broader reliability or compatibility qualification.

Detailed measurements and reviews belong in the [test records](../tests/records/README.md).
Outstanding reliability and compatibility limits remain in [qualification gaps](../tests/qualification-gaps.md).
Screenshots and demonstrations do not close those gaps.
