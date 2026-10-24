# Firmware Development and Testing

This guide summarizes firmware build and test workflows for RP2040 and RP2350. The canonical C test placement and
hardware limits are in [firmware test guidance](../../firmware/tests/README.md) and the
[test ownership index](../../tests/README.md). System and layer boundaries are in the
[architecture guide](../design/architecture.md); the software HID protocol is implemented across firmware and host
clients, while physical USB behavior remains a hardware-qualification item. The
[HID Report Reference](../design/usb/hid-report-reference.md) is authoritative for the current contract.

## Prerequisites

- Git with submodule support
- CMake, Ninja (recommended), and the Arm GNU toolchain (`arm-none-eabi-gcc`)
- Python 3.10+ with `venv` support for coverage tooling and repository tests
- Ruby 3+, GCC, and matching `gcov` for Ceedling tests
- The Pico SDK and FreeRTOS submodules pinned by the repository

Initialize the required submodules from the repository root:

```sh
git submodule update --init --recursive
```

## Build Firmware

Use the project wrapper from the repository root. It selects the pinned SDK and FreeRTOS submodules by default and
creates a separate build directory for the selected board and storage transport:

```sh
tools/firmware/build.sh --board pico --sd-card-transport SDIO_PIO
tools/firmware/build.sh --board pico2 --sd-card-transport SPI
```

Supported board values are `pico` and `pico2`; transports are `SDIO_PIO` and `SPI`. Use a different `--build-dir`
when comparing configurations. The default image uses the SDK stock clock. A successful cross-build does not qualify
physical USB, GPIO, card timing, or storage reliability.

## Firmware Unit Tests

Ceedling tests exercise host-testable C modules with Unity, CMock, and scoped SDK/RTOS fakes. Initialize the Embedded
CLI submodule and let the wrapper install/check the configured test tools:

```sh
git submodule update --init firmware/third_party/embedded-cli
tools/validation/setup-ceedling-testing.sh check
tools/validation/setup-ceedling-testing.sh test:all
```

Run one focused test by its file basename, without `.c`:

```sh
tools/validation/setup-ceedling-testing.sh test:test_sd_protocol
```

Tests belong under `firmware/tests/`, mirroring the production source subtree. Keep test ownership with the module or
application service whose behavior is under test. The firmware test guide documents naming, fixtures, coverage, and
the boundary between host fakes and real hardware behavior.

## Repository Validation

Repository contracts cover cross-project configuration and build artifacts; they are distinct from firmware unit
tests. Install their Python test dependencies, then run the contract subset:

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
```

The complete project validation workflow runs shell checks, repository contracts, Ceedling checks and tests, SDIO PIO
timing models, the RP2040/RP2350 × SDIO/SPI FreeRTOS build matrix, and MSC artifact checks:

```sh
tools/validation/validate.sh
```

Use `tools/validation/validate.sh --help` for options to restrict a board or skip a validation phase. PIO timing models
need `pioasm` from an SDIO_PIO build. The
[test ownership index](../../tests/README.md) lists inputs and limits for each test class.

## Hardware Validation

Host tests and cross-builds do not establish physical behavior. Board validation must use the intended board, card,
breakout, wiring, and exact firmware image. Follow the [hardware setup guide](../hardware-setup.md), storage
[transport and qualification notes](../design/sdcard/sdcard-design.md), and the specific procedure for the behavior
under test. Record partial runs as partial; passing tests do not imply sustained card integrity, safe power-loss
recovery, or full MSC host compatibility.
