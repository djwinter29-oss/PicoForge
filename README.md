# PicoForge

[![PR Check](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml/badge.svg?branch=main)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml)
[![License: BSD 3-Clause](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)
![Targets: RP2040 and RP2350](https://img.shields.io/badge/Targets-RP2040%20%7C%20RP2350-2674b8)

PicoForge is firmware for Raspberry Pi Pico and Pico 2 boards. Connect a board
over USB to monitor its status, control its LED, and work with files on an
attached SD card using a dashboard or serial console.

**Status:** under active development. Back up important card data before use.
High-speed SD-card operation depends on the board, card, and wiring; a short
successful test does not guarantee sustained reliability.

## What You Can Do

- View board identity, uptime, system clock, temperature, and SD-card information.
- Control the board LED from a dashboard.
- Browse, read, and manage files on FAT/exFAT SD cards through the USB console.
- Share an attached SD card with your computer as a USB drive.
- Run an SD-card write/read self-test with progress, verification results, and
  measured transfer speeds.

SD cards can use SDIO or SPI. An external SD-card breakout is required for
storage features; it is not required for basic board monitoring.

## What You Need

- A Raspberry Pi Pico or Pico 2 and a USB data cable.
- For storage: a compatible 3.3 V SD-card breakout and an SD card.
- To build firmware: Git, CMake, Ninja, and the Arm GNU toolchain
  (`arm-none-eabi-gcc`).
- For the Python dashboard: Python 3.10+ with virtual-environment support.

Follow the [hardware setup guide](docs/hardware-setup.md) before connecting an
SD-card breakout. Use 3.3 V logic and a common ground, never 5 V.

## Get Started

### 1. Build the Firmware

Clone the repository, then run these commands from its root:

```sh
git submodule update --init --recursive
tools/firmware/build.sh --board pico2
```
tools/firmware/build.sh --board pico2 --sd-card-transport SDIO_PIO
For the original Pico, use `--board pico` instead. The build uses the Pico SDK
included with the project.
Choose `--board pico` or `pico2`, and `--sd-card-transport SPI` or `SDIO_PIO`.
The four board/transport combinations build as FreeRTOS applications using the
Pico SDK included with the project. Each combination has its own directory:
`build/firmware-<board>-<transport>`.
`pico_forge_rp2350_250_20261018.uf2`. The date is UTC at CMake configuration time; the build script reconfigures
on each run. Reconfigure an existing directory before a new day's direct `cmake --build` to refresh its date.
Release packages and tag validation are documented in the [release guide](docs/release.md).

Standard builds use the SDK stock clock: 125 MHz for RP2040 and 150 MHz for RP2350. Use the dashboard's
System clock setting to select a supported runtime clock; changes take effect after reboot.
ELF, UF2, BIN, HEX, map and disassembly outputs share the basename. The CMake target is `pico_forge`.
`pico_forge_elf.txt` in the build directory records the exact ELF path for flashing and validation;
tools never select an image by wildcard or modification time. Use separate build directories for different
boards or SD transports: those distinctions are not all encoded in the requested filename.

### 2. Flash Your Board

1. Hold **BOOTSEL** while connecting the board to USB.
2. Copy the generated `pico_forge_rp2350_150_<YYYYMMDD>.uf2` from `build/firmware-pico2-SDIO_PIO/` to that drive.
3. The board restarts automatically.

For a Pico build, use `pico_forge_rp2040_125_<YYYYMMDD>.uf2` from `build/firmware-pico-SDIO_PIO/`.
Always use the image built for your board.

### 3. Open the Dashboard

```sh
tools/host/run-python.sh
```

The launcher creates its Python environment and installs dependencies on first
use. Open **http://127.0.0.1:5000/** to view and control the connected board.
Use **Rescan** and the Device identity board selector to switch between boards.
Python and .NET both default to port 5000; run one at a time or pass a different port to either launcher.
Use only one dashboard connection per board at a time.

On Linux, if HID access is denied, run:

```sh
tools/host/hid-permissions-setup.sh
```

Then reconnect the board and restart the dashboard.

## Other Ways to Connect

| Interface | Start | Open |
| --- | --- | --- |
| .NET dashboard (.NET 10 SDK required) | `tools/host/run-dotnet.sh` | http://127.0.0.1:5000/ |
| Direct browser dashboard | `tools/host/run-webhid.sh` | http://localhost:8000/ |
| USB serial console | Open the PicoForge CDC port in a serial terminal | Enter `help` to list commands |

The direct browser dashboard requires a WebHID-capable browser and permission
to access the device. For the serial console, choose the PicoForge USB port,
not a Debug Probe's UART port. See the [console guide](docs/cdc-cli.md) for
terminal settings and available commands.

## Using an SD Card

With a correctly wired card, firmware attempts to mount its existing FAT/exFAT
filesystem automatically. It does not format the card.

Useful console commands:

```text
sd status
fs ls /
fs cat /example.txt
sd diagnostics
```

To use the card as a USB drive:

1. Finish any file operations and run `fs unmount` in the console.
2. Enable USB mass storage (MSC) in the dashboard. USB briefly reconnects.
3. Use the drive on your computer.
4. Safely eject the drive, then disable MSC in the dashboard before returning
   to firmware file access.

The computer and firmware cannot access the card's files at the same time.
Do not unplug or remove the card during writes.

The dashboard's [SD-card self-test](docs/sd-self-test.md) writes and verifies a
temporary `/PFTEST.BIN` file and **replaces any existing file at that path**.
Selecting Start authorizes those writes. Back up your data first; this is not a filesystem repair or a
guarantee of card reliability.

## Help and Documentation

**Set up and use**

- [Hardware setup](docs/hardware-setup.md): wiring, supported connections, and board notes.
- [Console guide](docs/cdc-cli.md): file commands, mounting, and recovery.
- [SD-card self-test](docs/sd-self-test.md): running tests and understanding results.

**Design reference** (current implementation)

- [Persistent settings](docs/settings.md): flash settings record, reserved sector, and API.
- [Architecture](docs/architecture.md): application layers, runtime, and storage ownership.
- [SD-card design](docs/sdcard-design.md): driver interfaces, initialization, and transfer policy.
- [Physical transports](firmware/src/drivers/storage/README.md): SDIO-PIO, SD-SPI, and card wiring.
- [USB protocol](docs/usb-protocol.md): HID reports, commands, and host compatibility.

**Validation and history**

- [Test records](docs/tests/records/README.md): dated measurements and reviews; not current-firmware qualification.
- [SD remediation matrix](docs/tests/sd-design-remediation.md): current review dispositions.

**Project**

- [Contributing](CONTRIBUTING.md): development and validation guidance.
- [Security reporting](SECURITY.md): report vulnerabilities privately.

## License

PicoForge is licensed under the [BSD 3-Clause License](LICENSE).
Third-party components retain their own licenses.
