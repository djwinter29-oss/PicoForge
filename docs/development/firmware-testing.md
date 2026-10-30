# Firmware Development and Testing

This guide summarizes firmware build and test workflows for RP2040 and RP2350. The canonical C test placement and
hardware limits are in [firmware test guidance](../../firmware/tests/README.md) and the
[test ownership index](../../tests/README.md). System and layer boundaries are in the
[architecture guide](../design/architecture.md); the software HID protocol is implemented across firmware and host
clients, while physical USB behavior remains a hardware-qualification item. The
[HID Report Reference](../design/usb/usb-hid-report-reference.md) is authoritative for the current contract.

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
tools/firmware/build.sh --board pico --sdcard-transport SDIO_PIO
tools/firmware/build.sh --board pico2 --sdcard-transport SPI
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
breakout, wiring, and exact firmware image. Follow the [hardware setup guide](../guides/hardware-setup.md), storage
[transport and qualification notes](../design/sdcard/sdcard-driver-design.md), and the specific procedure for the behavior
under test. Record partial runs as partial; passing tests do not imply sustained card integrity, safe power-loss
recovery, or full MSC host compatibility.

### Storage Lifecycle Board Checks

This is the hardware acceptance procedure for the
[default-mounted storage design](../design/architecture.md#storage-ownership-lifecycle), not a record of passing tests.
Use backed-up or expendable FAT/exFAT media and record the board, card, transport, firmware image, host OS, and client.
Exercise the supported RP2040/RP2350 and SPI/SDIO configurations; one board/OS pass does not qualify the others.

1. Boot with a valid card and no dashboard/CDC session. Connect afterward and verify fresh `mounted` storage status
   without sending `fs mount` or Start. Read an existing file, then create/read/delete a disposable file via CDC.
2. Start one small self-test from mounted idle status. Verify that the first click runs write/verify/cleanup rather
   than only mounting, and that storage remains mounted afterward. Record command-acceptance delay separately from
   write/read timing; free-space scanning is outside the measured transfer phases.
3. Enable USB sharing directly from mounted idle status without manual unmount. Verify worker unmount precedes host
   block access, the host can use the card, and firmware file access/self-test are refused throughout host ownership.
   Check rejection while a storage command or self-test is outstanding; do not interrupt live card writes.
4. Safely eject through the host OS, then use normal disable (opcode `3`, action `0`) and verify automatic remount
   without Start or `fs mount`. Read a disposable file changed by the host to check that stale filesystem state
   was discarded. Do not treat HID reconnection or transition completion as mounted readiness.
5. Separately test the dashboard's confirmed force-return action using expendable media with host writes stopped.
   Verify its one-attempt remount result and failure block; it is not a substitute for the normal safe-return check.
   Cancel its dialog and verify no command is sent.
6. Boot without a card and with unsupported/unmountable expendable media. Verify USB management remains responsive,
   unavailable/error status is explicit, Start remains disabled, and no formatting occurs. Check paced retries and,
   where card detect is wired, insertion/removal while idle. Never remove media during a write.
7. Explicitly unmount and verify idle polling does not immediately remount. Verify a deliberate mount or normal
   ownership-return event restores normal use. Exercise fault recovery only with an appropriate fault fixture;
   confirm it remains unmounted for inspection and does not replay failed writes or clear its recovery block on
   insertion/normal return.

Record preparation/status visibility gaps separately from storage correctness. Richer mounting/checking UI labels
remain a design requirement until implemented and tested; existing wire states must not be reinterpreted to fake
those phases. Host mocks cannot establish real flush/eject behavior or filesystem integrity after forced return.
