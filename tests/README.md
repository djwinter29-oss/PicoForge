# Test Ownership

This directory structure separates firmware unit tests, repository-level contracts, and hardware test documentation. Keep tests in the location that matches what they verify; do not treat a host pass as hardware qualification.

## Firmware Unit Tests

`firmware/tests/` is the home for Ceedling tests of hardware-independent C modules under `firmware/src/platform/`, `firmware/src/drivers/`, and `firmware/src/middleware/`.

- Name test files `test_*.c`.
- Use Unity assertions and CMock for hardware-facing collaborators.
- Keep Pico SDK, board support, timing, GPIO, USB, and SDIO behavior out of host-only tests; cover those on target or with hardware validation.
- Use the repository-root `project.yml` and `tools/validation/setup-ceedling-testing.sh` to configure and run Ceedling.

No firmware unit-test files have been added yet; `firmware/tests/README.md` documents the setup and commands.

## Repository Contracts

`tests/contracts/` is for host-runnable checks across repository boundaries, such as consistency between build configuration, CI workflows, dependency pins, and published artifacts. These are not module unit tests; keep C module tests in `firmware/tests/`.

No contract tests have been added yet.

## Host Applications

Host-side tests live with their implementation: Python tests under `host/python/tests/`, .NET tests under `host/dotnet/tests/`, and browser protocol tests under `host/webhid/tests/`. These tests validate HID protocol decoding and status presentation without requiring a connected board.

## Hardware Test Documentation

`docs/tests/` is for hardware test plans, fixture and wiring procedures, and qualification evidence. Procedures requiring a board or probe should state the hardware setup and distinguish a partial run from a complete pass.

## Project Validation

Run `tools/validation/validate.sh` from the repository root for shell syntax checks, Ceedling project validation and available unit tests, and RP2040/RP2350 firmware builds. Run `tools/validation/coverage-firmware.sh` when firmware unit tests exist to generate the configured coverage reports.