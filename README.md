# PicoForge

[![PR Check](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml/badge.svg?branch=main)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml)
[![Release](https://github.com/djwinter29-oss/PicoForge/actions/workflows/release.yml/badge.svg)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/release.yml)
[![License: BSD 3-Clause](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)
![Targets: RP2040 and RP2350](https://img.shields.io/badge/Targets-RP2040%20%7C%20RP2350-2674b8)

**Monitor your Pico. Manage its SD card. Connect with one USB cable.** PicoForge adds board status, LED control, an
interactive storage console, and optional USB-drive access to Raspberry Pi Pico and Pico 2 boards. Use a browser,
Python, or .NET dashboard, or work directly through the USB serial console.

[Firmware and host downloads](https://github.com/djwinter29-oss/PicoForge/releases/latest) ·
[WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) ·
[Hardware setup](docs/guides/hardware-setup.md) ·
[Python package](https://pypi.org/project/picoforge/)

Publication links require the initial
[release setup](docs/deployment/release.md#publishing-setup) and may be unavailable until the first successful publication.

**Status:** under active development. Back up important card data before use. A successful build or short test does not
qualify sustained SD-card reliability, USB mass-storage compatibility, or power-loss recovery.

## Why PicoForge?

| | What you get |
| --- | --- |
| **Board status at a glance** | Board identity, uptime, system clock, temperature, runtime metrics, and SD-card state. |
| **Storage without removing the card** | Browse and manage FAT/exFAT files through the USB console. |
| **Use the card from your computer** | Temporarily share an attached card as a USB mass-storage drive, with explicit ownership handoff. |
| **Check card I/O** | Run a dashboard self-test that writes a temporary file, verifies its contents, and reports measured speeds. |
| **Choose your interface** | Use the hosted WebHID dashboard, local Python or .NET dashboard, or USB serial console. |

SD storage uses either SDIO-PIO or SPI, selected when building firmware. An external SD-card breakout is required for
storage features; basic board monitoring and LED control do not require one.

## Get Started

### 1. Get or Build the Firmware

Download the `.uf2` package for your board and SD transport from
[Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest), or build from a source checkout. Firmware
builds require Git with submodule support, CMake, Ninja, and `arm-none-eabi-gcc`.

From the repository root:

```sh
git submodule update --init --recursive
tools/firmware/build.sh --board pico2 --sdcard-transport SDIO_PIO
```

Choose `--board pico` for the original Pico and `--board pico2` for Pico 2. Choose `--sdcard-transport SPI` or
`SDIO_PIO` to match your breakout wiring. The build creates a separate directory for each board/transport pair, such
as `build/firmware-pico2-SDIO_PIO/`. Stock-clock Pico 2 builds produce a dated image named
`pico_forge_rp2350_150_<YYYYMMDD>.uf2`; Pico builds use `pico_forge_rp2040_125_<YYYYMMDD>.uf2`. See the
[release guide](docs/deployment/release.md) for published packages and build configurations.

### 2. Flash the Board

1. Hold **BOOTSEL** while connecting the board to your computer over USB.
2. Copy the `.uf2` built for your board to the USB drive that appears.
3. The board restarts automatically.

### 3. Connect the Hardware

For storage, wire a compatible 3.3 V SD-card breakout and card using the
[hardware setup guide](docs/guides/hardware-setup.md). Use 3.3 V logic and a common ground; never connect 5 V logic.
Basic monitoring works without the SD-card hardware.

### 4. Connect a Dashboard

The hosted [WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) runs in desktop Chrome or Edge. Click
**Connect** and select the board; no host installation is needed. Alternatively, install the Python package:

```sh
python -m pip install picoforge
picoforge web
```

The Python dashboard opens at **http://127.0.0.1:5000/**. Python 3.10 or newer is required. A self-contained .NET host
is also available from [Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest) for Windows x64, Linux
x64, and macOS ARM64. Extract the full archive and run `PicoForge.exe` on Windows or `./PicoForge` on Linux/macOS.
Native HID dependencies and operating-system permissions still apply.

## Use the SD Card

With a correctly wired card, firmware attempts to mount its existing FAT/exFAT filesystem automatically. It does not
format the card on mount. Use the USB serial console for file operations:

```text
storage status
fs ls /
fs cat /example.txt
```

To share the card with your computer:

1. Finish file operations and enter `fs unmount` in the console.
2. Enable USB mass storage (MSC) in the dashboard; USB briefly reconnects.
3. Use the card from your computer.
4. Safely eject the drive, then disable MSC in the dashboard before returning to firmware file access.

The computer and firmware must not access the card at the same time. Do not remove the card during writes. The dashboard
self-test writes and verifies `/PFTEST.BIN`; it refuses to overwrite an existing file at that path. Starting a test
authorizes card writes and contributes to card wear. Back up important data first. The test is not filesystem repair
or a guarantee of card reliability. Read the [self-test guide](docs/guides/sdcard-performance-test.md) before use.

## Dashboards and Console

### Browser Dashboard

Use the hosted [WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome or Edge. Connect the
board to the computer running the browser and grant access when prompted. See the [WebHID guide](host/webhid/README.md)
for local hosting and connection details.

### Local Python Dashboard

Install [picoforge](https://pypi.org/project/picoforge/) and run `picoforge web`, as shown above. From a source
checkout, `tools/host/run-python.sh` creates an environment and installs dependencies on first use. See the
[Python host guide](host/python/README.md).

### .NET Dashboard

Download a self-contained host from [Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest), or run
`tools/host/run-dotnet.sh` with the .NET 10 SDK from a source checkout. See the
[.NET host guide](host/dotnet/PicoForge/README.md).

### USB Serial Console

Open the PicoForge USB CDC port in a serial terminal and enter `help` to list commands. Choose the PicoForge CDC port,
not a Debug Probe's UART port. See the [console guide](docs/guides/usb-cdc-cli.md) for terminal settings, commands,
and recovery behavior.

Python and .NET dashboards both default to port 5000; run one at a time or choose another port. Use only one dashboard
connection per board at a time. On Linux, if HID access is denied, run `tools/host/hid-permissions-setup.sh`, then
reconnect the board and restart the dashboard.

## Hardware Compatibility

PicoForge targets the standard Raspberry Pi Pico (RP2040) and Pico 2 (RP2350). The SD-card transport is selected at
build time, not when connecting the dashboard. Use the hardware setup guide to verify the breakout pinout and board
connections before enabling storage.

PicoForge's reference Pico-family monitor UART wiring follows the
[PicoUart board setup](https://github.com/djwinter29-oss/PicoUart/blob/main/docs/hardware-setup.md) approach. In
PicoForge, UART0 is used for independent debug logging; it is not a multi-port UART bridge.

## Before You Connect

- GPIO and SD-card signals are **3.3 V only**. Never apply 5 V or RS-232 levels directly.
- Mount only cards with data you have backed up. PicoForge does not format a card during automatic mounting.
- Do not access files from the computer and firmware simultaneously. Safely eject before disabling MSC.
- Do not unplug or remove a card during writes or a self-test.
- High-speed SD-card operation depends on the board, card, breakout, wiring, and clock. A short successful test does
  not guarantee sustained reliability.
- Physical USB/MSC workflows and power-loss recovery are not fully qualified. See the
  [test records](docs/tests/records/README.md) for the scope and limits of existing measurements.

## Documentation

**Set up and use**

- [Hardware setup](docs/guides/hardware-setup.md): supported boards, wiring, power, and bring-up.
- [USB CDC console guide](docs/guides/usb-cdc-cli.md): terminal setup, file commands, mounting, and recovery.
- [SD-card self-test guide](docs/guides/sdcard-performance-test.md): safe test procedure and result interpretation.

**Design and implementation**

- [Architecture](docs/design/architecture.md): firmware layers, runtime, and storage ownership.
- [Persistent settings](docs/design/settings.md): supported profiles, persistence, and API.
- [SD-card driver design](docs/design/sdcard/sdcard-driver-design.md): driver layers, protocol, and ownership.
- [SD-card physical links](docs/design/sdcard/sdcard-physical-links.md): transport engines, wiring, and clock planning.
- [USB HID report reference](docs/design/usb/usb-hid-report-reference.md): report layouts and command semantics.

**Development and validation**

- [Firmware development and testing](docs/development/firmware-testing.md): build, unit tests, and hardware limits.
- [Host tools development](docs/development/host-tools.md): dashboards, tests, and shared frontend boundaries.
- [Test records](docs/tests/records/README.md): dated measurements and reviews, not current-firmware qualification.
- [SD remediation matrix](docs/tests/sd-design-remediation.md): current review dispositions.
- [Release guide](docs/deployment/release.md): packages, publication setup, and release validation.
- [Contributing](CONTRIBUTING.md): development and validation guidance.
- [Security reporting](SECURITY.md): report vulnerabilities privately.

## License

PicoForge is licensed under the [BSD 3-Clause License](LICENSE). Third-party components retain their own licenses.
