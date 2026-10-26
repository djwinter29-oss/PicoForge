# PicoForge

[![PR Check](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml/badge.svg?branch=main)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml)
[![Release](https://github.com/djwinter29-oss/PicoForge/actions/workflows/release.yml/badge.svg)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/release.yml)
[![License: BSD 3-Clause](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)
![Targets: RP2040 and RP2350](https://img.shields.io/badge/Targets-RP2040%20%7C%20RP2350-2674b8)

**Low-cost Pico hardware. Less embedded integration work.**

PicoForge is an integrated firmware-and-host platform for Raspberry Pi Pico-family boards, unifying device
monitoring, configuration, and control through a single management interface. It delivers practical
board-management capabilities today while providing an extensible foundation for embedded diagnostics, automation,
and future device integrations.

Board management and SD storage establish the foundation; host-controlled peripheral access is the next direction.

[Firmware and host downloads](https://github.com/djwinter29-oss/PicoForge/releases/latest) ·
[WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) ·
[Getting started](docs/guides/getting-started.md) ·
[Python package](https://pypi.org/project/picoforge/)

Publication links may be unavailable until the first successful publication; the getting-started guide also covers
source setup.

## Why PicoForge?

A Pico provides hardware interfaces for sensors and peripherals, but using them from a computer takes additional
work: toolchain setup, firmware development, flashing, device communication, and host integration. That work can
take longer than the experiment itself.

PicoForge combines low-cost Pico hardware with an integrated firmware-and-host system to reduce that repeated work.
Its goal is to bring sensors and peripherals into Python or C# applications on Linux, Windows, and macOS without
custom embedded firmware for every experiment. Board management and storage are available today; peripheral bridges
and public host APIs are planned.

## Use It or Extend It

- **Use it:** flash firmware and manage the board and attached storage through a dashboard or USB console.
- **Extend it:** build on the Pico SDK, FreeRTOS, and existing firmware/host interfaces for a specialized application.
  SD transport is selectable today; broader capability selection and peripheral host libraries are future goals.

## Available Today

| Capability | What you get today |
| --- | --- |
| **Board status at a glance** | Board identity, uptime, system clock, temperature, runtime metrics, and SD-card state. |
| **Configuration and control** | Board LED control, reboot, and persistent system-clock and storage-speed settings. |
| **Storage without removing the card** | Browse and manage FAT/exFAT files through the USB console. |
| **Use the card from your computer** | Temporarily share an attached card as a USB mass-storage drive, with explicit ownership handoff. |
| **Check card I/O** | On supporting firmware, run a dashboard self-test that writes a temporary file, verifies its contents, and reports measured speeds. |
| **Choose your interface** | Use the hosted WebHID dashboard, local Python or .NET dashboard, or USB serial console. |

SD storage uses either SDIO-PIO or SPI, selected when building firmware. An external SD-card breakout is required for
storage features; basic board monitoring and LED control do not require one.

USB HID carries management commands and status, CDC provides the console, and optional MSC provides host block access.
The browser, Python, and .NET dashboards are companion interfaces to the same firmware.

## See It in Action

![Python dashboard connected to a Pico 2, showing board controls, live diagnostics, and SDIO storage](docs/images/dashboard-python.jpg)

The Python dashboard connected to a real Pico 2: live board telemetry, configuration controls, and SDIO/exFAT storage.
See the [capture details](docs/development/demonstrations.md#captured-dashboard) for the recorded configuration and scope.

## Project Direction

The next intended enabler is one host-accessible I²C master and one SPI master, supported by Python and C# libraries.
PWM is a candidate extension, and broader selectable builds would let developers retain only the capabilities they
need. Wireless access remains an optional future direction, not a committed feature.

See the [project vision](docs/design/vision.md) for intended workflows and scope, and the
[demonstration plan](docs/development/demonstrations.md) for walkthroughs and examples still to add.
These planned capabilities are not presented as implemented.

## Get Started

1. Get firmware for your board and SD transport from [Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest),
   or build it from source.
2. Flash the board and, if using storage, connect a compatible 3.3 V SD-card breakout.
3. Connect through WebHID, a local Python or .NET dashboard, or the USB serial console.

Follow the [getting-started guide](docs/guides/getting-started.md) for build commands, flashing, host setup, storage
workflows, and safety precautions. GPIO signals are **3.3 V only**. Back up card data, never allow simultaneous
firmware/host file access, and safely eject before returning USB storage to the board.

## Documentation

**Set up and use**

- [Getting started](docs/guides/getting-started.md): firmware, flashing, dashboard connections, and first storage operations.
- [Hardware setup](docs/guides/hardware-setup.md): supported boards, wiring, power, and bring-up.
- [WebHID](host/webhid/README.md), [Python](host/python/README.md), and [.NET](host/dotnet/PicoForge/README.md):
  host-specific setup and usage.
- [USB CDC console guide](docs/guides/usb-cdc-cli.md): terminal setup, file commands, mounting, and recovery.
- [SD-card self-test guide](docs/guides/sdcard-performance-test.md): safe test procedure and result interpretation.

**Design and implementation**

- [Project vision](docs/design/vision.md): developer experience, intended capabilities, and scope boundaries.
- [Architecture](docs/design/architecture.md): firmware layers, runtime, and storage ownership.
- [Persistent settings](docs/design/settings.md): supported profiles, persistence, and API.
- [SD-card driver design](docs/design/sdcard/sdcard-driver-design.md): driver layers, protocol, and ownership.
- [SD-card physical links](docs/design/sdcard/sdcard-physical-links.md): transport engines, wiring, and clock planning.
- [USB HID report reference](docs/design/usb/usb-hid-report-reference.md): report layouts and command semantics.

**Development and validation**

- [Demonstration plan](docs/development/demonstrations.md): screenshots, walkthroughs, and runnable examples to add.
- [Firmware development and testing](docs/development/firmware-testing.md): build, unit tests, and hardware limits.
- [Host tools development](docs/development/host-tools.md): dashboards, tests, and shared frontend boundaries.
- [Qualification gaps](docs/tests/qualification-gaps.md): development status and limits of existing validation.
- [Test records](docs/tests/records/README.md): dated measurements and reviews, not current-firmware qualification.
- [SD remediation matrix](docs/tests/sd-design-remediation.md): current review dispositions.
- [Release guide](docs/deployment/release.md): packages, publication setup, and release validation.
- [Contributing](CONTRIBUTING.md): development and validation guidance.
- [Security reporting](SECURITY.md): report vulnerabilities privately.

## License

PicoForge is licensed under the [BSD 3-Clause License](LICENSE). Third-party components retain their own licenses.
