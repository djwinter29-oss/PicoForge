# PicoForge

PicoForge is a production-ready application framework for building embedded products on the RP2040 and RP2350 microcontroller families. It brings reusable drivers, platform services, middleware, testing infrastructure, and CI/CD workflows together in a modular foundation for product development.

The goal is to make common embedded capabilities easier to reuse across projects, while keeping application code organized around clear platform and middleware components.

## At A Glance

- **Target platforms:** RP2040 and RP2350
- **Platform services:** board support, version management, build configuration, configuration management, and logging
- **Middleware:** FatFs, SDIO, command-line interface, storage, and USB services
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
│   │   └── main.c           # USB-stdio Hello World
│   ├── tests/               # Firmware unit tests
│   └── third_party/
│       ├── FreeRTOS-Kernel/ # Pinned FreeRTOS dependency submodule
│       └── pico-sdk/        # Pinned Pico SDK submodule
├── tests/
│   └── contracts/           # Cross-component and repository contract tests
└── tools/
	├── firmware/            # SDK setup and firmware build helpers
	├── host/                 # Host test environment setup helpers
	└── validation/          # Local validation scripts
```

## Firmware Setup

Install Git, CMake, Ninja, and the `arm-none-eabi-gcc` toolchain, then source the SDK setup script from the repository root:

```sh
git submodule update --init --recursive
. tools/firmware/setup-sdk-env.sh
tools/firmware/build.sh --board pico
tools/firmware/build.sh --board pico2
```

Pico SDK 2.3.0 and FreeRTOS-Kernel V11.3.1 are pinned as submodules under `firmware/third_party/`, including their nested submodules. The setup script validates the SDK submodule and exports `PICO_SDK_PATH`; set `PICO_SDK_PATH` or pass `--pico-sdk-path` to use another existing checkout. Firmware runs FreeRTOS on one core by default; see [docs/architecture.md](docs/architecture.md) for the core-ownership policy.

The current firmware is a FreeRTOS task-based USB-stdio Hello World for RP2040 (`pico`) and RP2350 (`pico2`). The build produces `picoforge_hello_world.uf2` under `build/firmware-pico/` or `build/firmware-pico2/`. To flash it, hold the board's BOOTSEL button while connecting USB, then copy the matching UF2 file to the mounted BOOTSEL drive. After reboot, connect to the board's USB serial interface to read the repeating greeting.

## Host Unit Tests

Ceedling 1.1.10 is configured at the repository root for firmware unit tests under `firmware/tests`. See [firmware/tests/README.md](firmware/tests/README.md) for setup and commands, and [tests/README.md](tests/README.md) for test ownership. The suite has no module tests yet; add the first when a reusable C component is introduced.

## Project Validation

Run `tools/validation/validate.sh` from the repository root to check shell syntax, validate and run Ceedling tests when present, and build both RP2040 (`pico`) and RP2350 (`pico2`) firmware targets. Use `--skip-firmware`, `--skip-tests`, or `--board NAME` for focused validation.

## Project Status

The repository currently contains the project overview, initial directory scaffold, a buildable single-core FreeRTOS Hello World firmware baseline, and Ceedling host-test infrastructure. Platform services, drivers, middleware, host unit tests, and release automation will be added as the framework is developed.
