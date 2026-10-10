# PicoForge

PicoForge is a production-ready application framework for building embedded products on the RP2040 and RP2350 microcontroller families. It brings reusable drivers, platform services, middleware, testing infrastructure, and CI/CD workflows together in a modular foundation for product development.

The goal is to make common embedded capabilities easier to reuse across projects, while keeping application code organized around clear platform and middleware components.

## At A Glance

- **Target platforms:** RP2040 and RP2350
- **Platform services:** board support, version management, build configuration, configuration management, and logging
- **Middleware:** FatFs, SDIO, command-line interface, storage, and USB services
- **Current USB device:** one CDC ACM interface and one HID status interface
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

See [docs/pinout.md](docs/pinout.md) for the hardware connection baseline and currently unassigned pin mappings.
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
│   │   │   └── sdio/        # PIO SD-card block-device API
│   │   ├── middleware/
│   │   │   └── storage/     # FatFs mount lifecycle API
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

The current firmware is a FreeRTOS-managed composite USB device for RP2040 (`pico`) and RP2350 (`pico2`). It exposes CDC ACM and vendor-defined HID interfaces. CDC sends a greeting when opened; HID reports product name, MCU, system clock, uptime, and USB/CDC state. Its VID/PID (`0xCAFE:0x4011`) is for development only and must be replaced before product release. See [docs/usb-protocol.md](docs/usb-protocol.md) for the report layout. The build produces `picoforge_hello_world.uf2` under `build/firmware-pico/` or `build/firmware-pico2/`. To flash it, hold the board's BOOTSEL button while connecting USB, then copy the matching UF2 file to the mounted BOOTSEL drive. Host enumeration still needs confirmation on physical boards.

## Host Tools

The Python host opens HID and serves a loopback-only dashboard:

```sh
uv venv build/picoforge-host
. build/picoforge-host/bin/activate
uv pip install -e host/python
picoforge-host status
picoforge-host web
```

The Python dashboard is at `http://127.0.0.1:8765/`. The .NET 10 host serves the same dashboard at `http://127.0.0.1:8766/`:

```sh
dotnet run --project host/dotnet/PicoForge.Host.csproj
```

For a direct browser connection, serve the WebHID page from localhost and open `http://localhost:8000/webhid/` in a WebHID-capable browser:

```sh
python3 -m http.server 8000 --directory host
```

WebHID requires a secure browser context and a user-granted device permission. CDC remains available to serial terminal applications.

## Host Tests

Ceedling 1.1.10 is configured at the repository root for firmware unit tests under `firmware/tests`. Host unit tests live beside each implementation and can be run together with:

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```

The script requires Python 3.10+, Node.js 22+, and .NET 10. The GitHub host-validation workflow installs these runtimes and runs the same script.

See [firmware/tests/README.md](firmware/tests/README.md) for Ceedling setup and [tests/README.md](tests/README.md) for test ownership.

## Project Validation

Run `tools/validation/validate.sh` from the repository root to check shell syntax, validate and run Ceedling tests when present, and build both RP2040 (`pico`) and RP2350 (`pico2`) firmware targets. Use `--skip-firmware`, `--skip-tests`, or `--board NAME` for focused validation.

## Project Status

The repository contains a buildable single-core FreeRTOS firmware baseline, composite CDC/HID status interface, Python and .NET HID hosts, a direct WebHID page, and Ceedling infrastructure. Reusable platform services, drivers, middleware, and release automation remain under development.
