# Test Ownership

This directory structure separates firmware unit tests, repository-level contracts, and hardware test documentation. Keep tests in the location that matches what they verify; do not treat a host pass as hardware qualification.

## Firmware Unit Tests

[firmware/tests/](../firmware/tests/) is the home for Ceedling tests of C modules under `firmware/src/`, including board profiles, drivers, middleware and USB callbacks.

- Name test files `test_*.c`.
- Use Unity assertions and CMock for hardware-facing collaborators.
- Use scoped SDK/RTOS fakes for host-testable behavior. Real GPIO, DMA, interrupt scheduling, USB traffic and electrical timing still require hardware validation.
- Use the repository-root `project.yml` and `tools/validation/setup-ceedling-testing.sh` to configure and run Ceedling.

See [firmware test guidance](../firmware/tests/README.md) for test placement, scoped fixtures and commands.

## Repository Contracts

`tests/contracts/` is for host-runnable checks across repository boundaries, such as consistency between build configuration, CI workflows, dependency pins, and published artifacts. These are not module unit tests; keep C module tests in `firmware/tests/`.

| Test | Purpose |
| --- | --- |
| [contracts/test_tinyusb_msc_contract.py](contracts/test_tinyusb_msc_contract.py) | Verify pinned TinyUSB source ordering and MSC linker-wrapper configuration |
| [test_validation_integrity.py](test_validation_integrity.py) | Exercise validation tools with copied sources and valid/broken ARM ELF fixtures |

Run from the repository root:

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
```

These tests require initialized TinyUSB sources and Arm GNU `nm`/`readelf`,
but do not compile firmware or require a connected board.

## PIO Instruction Models

[pio/test_sdio_pio_timing.py](pio/test_sdio_pio_timing.py) assembles the actual
SDIO PIO source and models TX timing and RX cadence. Run it from the repository
root after a firmware build, or provide `PIOASM`:

```sh
python3 -m pytest tests -m pio
```

This is an instruction model, not electrical or sustained-transfer qualification.

## Host Applications

Host-side tests live with their implementation: Python tests under `host/python/tests/`, .NET tests under `host/dotnet/tests/`, and browser protocol tests under `host/webhid/tests/`. These tests validate HID protocol decoding and status presentation without requiring a connected board.

## Build-Artifact Tests

These project-level tests assert properties of supplied cross-build artifacts:

| Test | Purpose |
| --- | --- |
| [artifacts/test_msc_elf.py](artifacts/test_msc_elf.py) | Verify MSC callback pointers in final ARM ELF artifacts |
| [artifacts/test_sd_ports.py](artifacts/test_sd_ports.py) | Verify bare-metal compilation manifests and linked symbols |

For existing Pico 2 validation builds, run from the repository root:

```sh
python3 -m pytest tests -m artifacts \
    --elf build/validation-pico2-SDIO_PIO/pf_hello_world.elf \
    --baremetal-build-dir build/validation-pico2-SDIO_PIO-baremetal
```

`validate.sh` invokes these tests after the corresponding firmware builds.
Bare-metal smoke targets have no USB interface and are not MSC checker inputs.
The MSC checker requires an unstripped ELF with DWARF and Arm GNU `nm`/`readelf`;
the port checker requires `compile_commands.json` and Arm GNU `nm`.
Neither test builds or flashes firmware.
Repeat either option to check multiple inputs. Selecting artifact tests without
the corresponding input fails explicitly; missing files are never silently skipped.

## Hardware Test Documentation

`docs/tests/` is for hardware test plans, fixture and wiring procedures, and qualification evidence. Procedures requiring a board or probe should state the hardware setup and distinguish a partial run from a complete pass.

## Project Validation

Install the pinned [pytest dependency](requirements.txt) in your Python environment
before running project validation. All project Python tests use pytest discovery,
registered markers and normal assertions. PIO timing combinations and artifact
inputs are parametrized so failures identify the individual case.
The root [pyproject.toml](../pyproject.toml) configures discovery and markers
using pytest 9's native `[tool.pytest]` table.

To run everything against existing builds, omit `-m` from the artifact command
above. For checks that do not require firmware builds, use `-m contracts`.
Use `-k NAME` to filter cases and `--junitxml=build/project-tests.xml` for CI reports.
Firmware CI runs contracts, integrity regressions, PIO models and MSC artifact
tests after each board build. The local validation runner additionally builds
and tests both bare-metal transports.

Run [validate.sh](../tools/validation/validate.sh) from the repository root for
shell syntax checks, these project tests, Ceedling tests, PIO models and
RP2040/RP2350 firmware builds. Run
[coverage-firmware.sh](../tools/validation/coverage-firmware.sh) for firmware coverage.

Keep test assertions and regression fixtures here; keep setup, orchestration
and benchmark analysis tools in [tools/validation/](../tools/validation/).