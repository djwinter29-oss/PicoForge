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
| **Lead I²C sensor example** | Configure one documented sensor and read equivalent measurements from Python and C#, with setup and transaction errors explained. | Select the sensor and wiring; implement the I²C bridge and supported host APIs first. |
| **Device-side recording example** | Configure a bounded acquisition from the host, record locally to SD, then retrieve and inspect the data. | Implement a defined acquisition workload with measured buffer limits and explicit overflow reporting. |
| **Developer extension example** | An application task using the shared dual-core FreeRTOS runtime and existing platform services, with host interaction and tests. | Document the supported build, resource ownership, synchronization, and observed service responsiveness; do not imply dedicated-core isolation. |

The I²C example is the lead developer demonstration, with the acceptance workflow defined in the
[project vision](../design/vision.md). Recording and task-based extensions are
optional follow-ons, not prerequisites for a successful sensor evaluation. Reuse the reference peripheral where
practical so developers can see what carries forward instead of assembling unrelated demonstrations.

## Evidence Requirements

- Use real screenshots and observed behavior, not mock results presented as hardware evidence.
- Runnable examples must use implemented APIs and identify required hardware, wiring, dependencies, and commands.
- State the firmware revision, board, host environment, and relevant peripheral or card configuration.
- Distinguish a successful walkthrough from broader reliability or compatibility qualification.

Detailed measurements and reviews belong in the [test records](../tests/records/README.md).
Outstanding reliability and compatibility limits remain in [qualification gaps](../tests/qualification-gaps.md).
Screenshots and demonstrations do not close those gaps.
