# Firmware Development and Testing

This guide summarizes firmware build and test workflows for RP2040 and RP2350. The canonical C test placement and
hardware limits are in [firmware test guidance](../../firmware/tests/README.md) and the
[test ownership index](../../tests/README.md). System and layer boundaries are in the
[architecture guide](../reference/architecture.md); the software HID protocol is implemented across firmware and host
clients, while physical USB behavior remains a hardware-qualification item. The
[HID Report Reference](../reference/usb-hid.md) is authoritative for the current contract.
W-board host checks and target builds do not qualify physical Wi-Fi/BLE; the
[wireless and BLE board checks](#wireless-and-ble-board-checks) are required for physical qualification.

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

Supported build-wrapper board values are `pico`, `pico_w`, `pico2`, and `pico2_w`; transports are `SDIO_PIO` and `SPI`.
Use a different `--build-dir`
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

The local validation wrapper runs shell checks, repository contracts, Ceedling checks and tests, SDIO PIO
timing models, the `pico`/`pico2` × SDIO/SPI FreeRTOS build matrix, and MSC artifact checks.
PR/release workflows additionally build `pico_w` and `pico2_w`; see the [release guide](../deployment/release.md).

```sh
tools/validation/validate.sh
```

Use `tools/validation/validate.sh --help` for options to restrict a board or skip a validation phase. PIO timing models
need `pioasm` from an SDIO_PIO build. The
[test ownership index](../../tests/README.md) lists inputs and limits for each test class.

## Hardware Validation

Host tests and cross-builds do not establish physical behavior. Board validation must use the intended board, card,
breakout, wiring, and exact firmware image. Follow the [hardware setup guide](../guides/hardware.md), storage
[transport and qualification notes](../reference/sdcard/driver.md), and the specific procedure for the behavior
under test. Record partial runs as partial; passing tests do not imply sustained card integrity, safe power-loss
recovery, or full MSC host compatibility.

### Storage Lifecycle Board Checks

This is the hardware acceptance procedure for the
[default-mounted storage design](../reference/architecture.md#storage-ownership-lifecycle), not a record of passing tests.
Use backed-up or expendable FAT/exFAT media and record the board, card, transport, firmware image, host OS, and client.
Exercise the supported RP2040/RP2350 and SPI/SDIO configurations; one board/OS pass does not qualify the others.

1. Boot with a valid card and no dashboard/CDC session. Connect afterward and verify fresh `mounted` storage status
   without sending `storage mount` or Start. Read an existing file, then create/read/delete a disposable file via CDC.
   Confirm CDC, HID and MSC enumerate with fixed interface numbers and MSC reports no medium while Pico owns the card.
2. Start one small self-test from mounted idle status. Verify that the first click runs write/verify/cleanup rather
   than only mounting, and that storage remains mounted afterward. Record command-acceptance delay separately from
   write/read timing; free-space scanning is outside the measured transfer phases.
3. Enable USB sharing directly from mounted idle status without manual unmount. Verify worker unmount precedes host
   block access, the host can use the card, and firmware file access/self-test are refused throughout host ownership.
   Check rejection while a storage command or self-test is outstanding; do not interrupt live card writes.
   Keep HID monitoring and a CDC session open across enable/return and verify neither disconnects nor restarts.
4. Safely eject through the host OS, then use normal disable (opcode `3`, action `0`) and verify automatic remount
   without Start or `storage mount`. Read a disposable file changed by the host to check that stale filesystem state
   was discarded. Verify MSC reports no medium while firmware uses the card. Re-enable sharing after firmware changes
   a disposable file and verify the host observes the new contents/capacity rather than cached metadata; inspect
   the SCSI Unit Attention/media-change response where host tracing is available.
   Do not treat HID availability or transition completion as mounted readiness.
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
8. On expendable media, stop host writes and safely eject while MSC remains enabled. Edit settings locally and
   verify the forced-save risk warning appears before **Save&Reboot**. Cancel/Escape must send no command.
   Confirm once; verify sync/detach precede flash programming, the original result is checked after HID reconnect
   without SET replay, no card I/O resumes before reset, and boot returns to automatic firmware mounting.
   Use controlled sync/detach/flash-failure fixtures to verify no reboot on failure and explicit diagnostics.
   Self-test/transition busy gates must still apply to forced saves.

Record preparation/status visibility separately from storage correctness. The protocol has no distinct
mounting/checking phase; existing wire states must not be reinterpreted to fake one.
Host mocks cannot establish real flush/eject behavior or filesystem integrity after forced return.

### Wireless and BLE Board Checks

Host checks cover settings-record validation, proof vectors and mutations, payload boundaries, missing keys,
restricted BLE writes, single-SET behavior, secret-free status and API errors. Wi-Fi state-machine tests cover boot
gating, association deadlines, reconnects, WPA3-SAE fallback, staged-credential expiry, scan cleanup, listener
retry pacing, and mDNS registration/removal. BLE checks cover report snapshots and read behavior. W-board builds do
not qualify physical radio behavior.

Use both Pico W and Pico 2 W with a test access point and expendable SD media. Record the firmware revision, board,
transport, host OS, and access point configuration. Do not use production credentials.

1. Exercise open, WPA2, WPA3-transition and WPA3-only joins, wrong WPA2/WPA3 passwords, DHCP timeout, link loss and
   reconnect. Verify scan, credential testing, staged-test link loss, Cancel, failed-test cleanup and the ten-minute
   staging expiry with the host connected and disconnected.
2. Verify BLE advertising and access with valid and invalid keys, rejected proofs, replayed commands, disconnect,
   reconnect and simultaneous Wi-Fi activity. Check scan data, pairing behavior, long reads, Database Hash visibility,
   advertising timing, Service Changed confirmation/timeout, and cache rediscovery. Record the client and OS.
   Confirm unauthorized writes remain rejected and BTstack does not write the settings sector.
3. Verify the TCP CLI is blocked when its saved switch is off after reboot, then enable it and confirm an authenticated
   client can connect. Also check that no saved key blocks clients. Verify hostname/DNS-SD resolution after DHCP and
   reconnect. Test a hostname conflict, no discovery on the test network, and disconnect while delayed mDNS replies
   are pending.
4. Repeat simultaneous Wi-Fi/BLE activity with USB storage ownership and storage benchmarks across SPI and SDIO
   configurations. Verify storage admission remains exclusive, capture task stack high-water observations, and
   record any USB, radio, or storage failures.

Record results in the [validation records](../validation/records/README.md). Partial runs and untested combinations
remain qualification gaps; host checks and target builds do not substitute for these board checks.

#### Direct Dashboard Board Checks

This is an acceptance procedure, not a passing hardware record. Run it on both Pico W (RP2040) and Pico 2 W (RP2350)
with the intended storage transport and browser. Record firmware/dashboard revisions, board, card/transport, host
OS, browser version, test access point, connection type, scenario, observed result, and pass/fail/not-run status.
Do not record Control keys or Wi-Fi passwords. Use test credentials and backed-up or expendable media.

1. Over USB, test a candidate Wi-Fi pair, then fail a replacement test. Verify the old pair is not saveable and the
   visible Save blocker requires another test or clearing. Cancel successfully and verify temporary staging clears.
   With a controlled cancellation failure, verify Save stays blocked until retry or a matching reconnect result;
   capture transaction evidence that reconnect does not resend cancellation.
2. Disconnect during scan/test/cancellation, reconnect the same board, and verify old progress/results cannot
   overwrite new drafts or unlock uncertain writes. Repeat with another board and verify its identity cannot
   resolve the original board's pending operation.
3. Test staging expiry with the dashboard open and disconnected. At expiry, verify Save is blocked and retesting is
   required. Do not shorten the firmware lifetime merely to qualify the normal ten-minute path.
4. Generate a replacement Control key, delay clipboard completion, then regenerate, disable Bluetooth, or complete
   a save. Verify late clipboard success/failure cannot overwrite newer feedback. With a controlled save failure,
   verify submitted key text is wiped, Save explains regeneration, and disabling releases that blocker. Verify the
   saved key is removed only after a successful save/reboot.
5. Connect over BLE with valid and invalid keys, disconnect/reconnect, and verify fresh status and permitted
   clock/storage/TCP CLI saves. Verify credential provisioning and storage ownership writes remain USB-only.
6. Run the [forced-save storage check](#storage-lifecycle-board-checks) with host writes stopped and safely ejected.
   Verify confirmation, same-board recovery of the original result without SET replay, and preservation of any
   newer dashboard draft. Do not infer storage integrity from a successful reconnect.

Use controlled diagnostic/fault fixtures for deterministic failures; do not interrupt production writes to induce
them. Keep browser-mock results separate from physical observations. An unavailable board, browser, or fault fixture
is `not-run`, not a pass, and remains in the [qualification gaps](../validation/qualification-gaps.md).

### Boot Settings Board Checks

Record the firmware commit, SDK/FreeRTOS revisions, board/MCU/flash model, transport, build clock, selected profiles,
and host OS. Use expendable media, stop SD writes, disable MSC, and prevent USB/probe back-power during interruption
tests.

1. In a diagnostic build, count erase/program callback entries and observe task counts and lockout-task reclamation
   without logging from the flash callback. Keep `flash_safe_execute()` exclusive to boot-config and keep the task
   population fixed.
2. From the USB task, alternate two settings for 100 saves without rebooting; verify each result, no stuck
   reclamation state, and task count returning to baseline. Repeat with CDC/HID traffic active, then reboot and check
   the final profiles. Use a diagnostic caller for the no-reboot loop because normal successful HID saves reboot.
3. Save identical settings 100 times and confirm the erase/program counters do not increase. Interrupt power at
   varied points during 20 changing-setting saves; after each restart, verify boot succeeds with a complete old/new
   record or complete defaults, never mixed fields. Save again to check recovery.
4. Record save counts, timings, USB disconnects/timeouts, reboot results, and errors.

### USB HID Board Checks

Exercise CDC, 69-byte Feature `20` transfers, 49-byte Report `12` transfers, HID, MSC transitions, and USB
re-enumeration on RP2040 and RP2350 hardware. Verify forced settings saves use the documented confirmation and
transaction flow, and that hosts never replay an uncertain command. Use the
[USB HID reference](../reference/usb-hid.md) for wire details and the storage lifecycle checks above for card
ownership behavior.

### SD-Card Transport Board Checks

For each supported board/card/transport combination, record card initialization, read/write self-test results,
effective bus clock, and task stack high-water marks. Exercise card removal during I/O, host eject and BOT reset
behavior, and power interruption during data and filesystem-metadata writes using expendable media. Read-only
initialization does not exercise high-speed writes; follow the
[SD-card performance procedure](../guides/sdcard-benchmark.md) for throughput measurements.
