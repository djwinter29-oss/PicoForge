# PicoForge

[![PR Check](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml/badge.svg?branch=main)](https://github.com/djwinter29-oss/PicoForge/actions/workflows/pr-check.yml)
[![License: BSD 3-Clause](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)
![Targets: RP2040 and RP2350](https://img.shields.io/badge/Targets-RP2040%20%7C%20RP2350-2674b8)

PicoForge is an evolving embedded firmware framework for RP2040 and RP2350, built on the Raspberry Pi Pico SDK and FreeRTOS. It brings together a buildable firmware baseline, USB CDC/HID interfaces, host-side tools, and a foundation for reusable drivers, middleware, and automated testing.

## At A Glance

- **Target platforms:** RP2040 and RP2350
- **Platform services:** board support, version management, build configuration, configuration management, and logging
- **Middleware:** FatFs, SDIO, command-line interface, storage, and USB services
- **Current USB device:** CDC ACM plus HID status, board telemetry, and LED control
- **Development workflow:** automated testing and CI/CD pipelines
- **Intended use:** industrial, IoT, and other embedded products

## Platform

The platform layer provides the shared foundation used by applications and middleware:

- **Board support:** common support for target hardware and board-level integration.
- **Version management:** a consistent way to track framework and product versions.
- **Build system:** shared build infrastructure for assembling embedded applications.
- **Configuration:** application and platform settings managed in a structured way.
- **Logging:** structured diagnostic output to help with development and field troubleshooting.

## Middleware

Reusable components provide common product functionality without requiring each application to build it from scratch:

- **FatFs:** file-system support for working with files on supported storage devices.
- **SDIO:** an interface for integrating SDIO-compatible peripherals and storage.
- **CLI:** a command-line interface for device interaction and diagnostics.
- **Storage:** reusable storage services for application data.
- **USB services:** common USB functionality for connected devices and host systems.

## Engineering Workflow

PicoForge is intended to support the full embedded development cycle, from bringing up a board to maintaining a product:

- **Modular drivers** help isolate hardware-specific behavior and encourage reuse.
- **Automated testing** helps catch regressions as platform services and middleware evolve.
- **CI/CD workflows** provide repeatable checks for changes and releases.
- **Structured configuration and logging** make products easier to adapt, inspect, and troubleshoot.

## Intended Applications

PicoForge is designed for teams building industrial controllers, connected IoT devices, and other embedded products that benefit from reusable platform services and a repeatable development workflow.

See [docs/pinout.md](docs/pinout.md) for Pico/Pico 2 header wiring, a candidate SD-card breakout map, and external QSPI flash constraints.
See [docs/architecture.md](docs/architecture.md) for implemented status and intended module boundaries.

## Repository Layout

The repository follows a firmware-focused layout, with documentation, tests, and development tooling kept separate from embedded source code:

```text
.
├── .github/workflows/       # Continuous integration and release workflows
├── docs/
│   ├── architecture.md      # Current state and intended module boundaries
│   ├── development/         # Build and contributor guidance
│   └── tests/               # Test plans and hardware test guidance
├── firmware/
│   ├── CMakeLists.txt       # Pico SDK firmware build
│   ├── src/
│   │   ├── integration/
│   │   │   ├── fatfs/      # FatFs configuration and disk-I/O adapter
│   │   │   ├── freertos/    # FreeRTOS configuration
│   │   │   └── usb/         # TinyUSB integration configuration
│   │   ├── drivers/
│   │   │   ├── system.c/h   # MCU identity, clock, and uptime
│   │   │   ├── temperature.c/h # Internal sensor and conversion
│   │   │   ├── led.c/h      # Board-configured LED control
│   │   │   └── sdio/        # Multi-file PIO SD-card block driver
│   │   ├── middleware/
│   │   │   └── storage/     # FatFs mount lifecycle API
│   │   ├── board/           # Board topology and SD-card configuration
│   │   ├── usb/             # HID API and TinyUSB descriptors
│   │   └── main.c           # Firmware startup and scheduler launch
│   ├── tests/               # Firmware unit tests
│   └── third_party/
│       ├── FreeRTOS-Kernel/ # Pinned FreeRTOS dependency submodule
│       ├── fatfs/           # FatFs R0.16 with official patches 1 and 2
│       └── pico-sdk/        # Pinned Pico SDK submodule
├── host/
│   ├── dotnet/              # .NET HID monitor and local dashboard host
│   ├── python/              # Python HID client and Flask dashboard host
│   ├── web/                 # Shared native-host status dashboard
│   └── webhid/              # Direct browser WebHID dashboard
├── tests/
│   └── contracts/           # Cross-component and repository contract tests
└── tools/
	├── firmware/            # SDK setup and firmware build helpers
	├── host/                 # Host test environment setup helpers
	└── validation/          # Local validation scripts
```

## Firmware Setup

Install Git, CMake, Ninja, and the `arm-none-eabi-gcc` toolchain, then initialize submodules and build from the repository root:

```sh
git submodule update --init --recursive
tools/firmware/build.sh --board pico
tools/firmware/build.sh --board pico2
```

Pico SDK 2.3.0 and FreeRTOS-Kernel V11.3.1 are pinned as submodules under `firmware/third_party/`, including their nested submodules. The build defaults to the SDK submodule; set `PICO_SDK_PATH` or pass `--pico-sdk-path` to use another existing checkout. Firmware runs FreeRTOS on one core by default; see [docs/architecture.md](docs/architecture.md) for the core-ownership policy.

The SD-card block driver is a PicoForge-owned 4-bit PIO SD memory-card host. Configure board-validated CMD and DAT0 pins before calling `picoforge_storage_mount()`; the host derives CLK and DAT1-DAT3 from DAT0. FatFs R0.16 is compiled and linked, but the example does not mount a volume or format media. See [the SDIO pinout constraints](docs/pinout.md) and the [FatFs integration notes](firmware/third_party/fatfs/README.md).

The current firmware is a FreeRTOS-managed composite USB device for RP2040 (`pico`) and RP2350 (`pico2`). It exposes CDC ACM and vendor-defined HID interfaces. CDC sends a greeting when opened; HID reports product identity, MCU, system clock, uptime, USB/CDC state, internal temperature, and board LED state, and accepts a set-LED command. The system, temperature, and LED drivers live directly in `firmware/src/drivers/`; multi-file drivers such as SDIO use grouped subdirectories. Its VID/PID (`0xCAFE:0x4011`) is for development only and must be replaced before product release. See [docs/usb-protocol.md](docs/usb-protocol.md) for the report layout. The build produces `picoforge_hello_world.uf2` under `build/firmware-pico/` or `build/firmware-pico2/`.

To flash over a CMSIS-DAP debug probe, run `tools/firmware/load.sh --board pico` or `tools/firmware/load.sh --board pico2`. Alternatively, hold BOOTSEL while connecting USB and copy the matching UF2 file to the mounted drive. Start the Python dashboard separately with `tools/host/run-python.sh` after installing its dependencies. Host enumeration and physical sensor/LED behavior still need board validation.

## Host Tools

The Python host opens HID and serves a loopback-only dashboard:

```sh
uv venv build/picoforge
. build/picoforge/bin/activate
uv pip install -e host/python
picoforge status
picoforge web
```

`tools/host/run-python.sh` starts the dashboard by default; pass `status` or `web` and its options to run a specific command. The dashboard is at `http://127.0.0.1:8765/`. The .NET 10 host serves the same dashboard at `http://127.0.0.1:8766/`:

```sh
tools/host/run-dotnet.sh
```

Set `DOTNET` to select a .NET 10 SDK executable. Application arguments can be passed through the script.

For a direct browser connection, serve the WebHID directory so the page opens at the origin root:

```sh
python3 -m http.server 8000 --directory host/webhid
```

Open `http://localhost:8000/` in a WebHID-capable browser, connect the board, and grant access to its HID interface.

WebHID requires a secure browser context and a user-granted device permission. CDC remains available to serial terminal applications.

## Host Tests

Ceedling 1.1.10 is configured at the repository root for firmware unit tests under `firmware/tests`. Host unit tests live beside each implementation and can be run together with:

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```

The script requires Python 3.10+, Node.js 22+, and .NET 10. The GitHub host-validation workflow installs these runtimes and runs the same script.

See [firmware/tests/README.md](firmware/tests/README.md) for Ceedling setup and [tests/README.md](tests/README.md) for test ownership.

## Project Policies

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution and validation guidance, [SECURITY.md](SECURITY.md) for private vulnerability reporting, and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community expectations and reporting concerns.

## Project Validation

Run `tools/validation/validate.sh` from the repository root to check shell syntax, validate and run Ceedling tests when present, and build both RP2040 (`pico`) and RP2350 (`pico2`) firmware targets. Use `--skip-firmware`, `--skip-tests`, or `--board NAME` for focused validation.

## Project Status

The repository contains a buildable single-core FreeRTOS firmware baseline, composite CDC/HID status interface, Python and .NET HID hosts, a direct WebHID page, and Ceedling infrastructure. Reusable platform services, drivers, middleware, and release automation remain under development.
