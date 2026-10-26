# Persistent settings

Settings live in one reserved flash sector. Each image has a compile-time transport (Pico/Pico 2 × SPI/SDIO_PIO);
there is no runtime transport selector.

| Setting | Values | Default | Applied |
| --- | --- | --- | --- |
| System clock (`pf_sysclk_profile_t`) | `PF_SYSCLK_DEFAULT`, `PF_SYSCLK_150MHZ`, `PF_SYSCLK_200MHZ`, `PF_SYSCLK_250MHZ` | Default: keep the SDK stock clock | At boot |
| Storage speed (`pf_storage_speed_t`) | `PF_STORAGE_SPEED_DEFAULT` (25 MHz), `PF_STORAGE_SPEED_HIGH` (50 MHz) | Default (25 MHz) | At boot |

Builds use the SDK stock clock; select a different clock through the saved system-clock profile, not a CMake
override. The former `PF_SYS_CLK_MHZ` option is rejected, including in existing caches. Clear it once with
`cmake -U PF_SYS_CLK_MHZ -S firmware -B <build-dir>`, then use the normal build command.

Both SD transports obtain their requested clock from
`pf_get_storage_speed_mhz()` in
[sd_board.c](../../firmware/src/drivers/sdcard/sd_board.c). The 25/50 MHz value is a target, not a guaranteed rate:
the hardware chooses the closest safe supported clock at or below it. Board data supplies wiring and controller
selection, not a separate speed. SPI above 25 MHz requires card CMD6 High-Speed support; otherwise the CSD-limited
Default-Speed rate is retained. SDIO High-Speed is limited by SYSCLK/4 and the card; there is no build-time
negotiation switch. Clear the removed cached option once with
`cmake -U PF_SDIO_PIO_HIGH_SPEED -S firmware -B <build-dir>` before rebuilding an existing configuration.
For example, a 50 MHz SDIO request at 150 MHz SYSCLK achieves 37.5 MHz. Status reports the actual bus clock separately.

## Using the API

```c
#include "platform/boot_config.h"

pf_sysclk_profile_t clk = pf_boot_config_sysclk();   /* saved value, or PF_SYSCLK_DEFAULT */
pf_storage_speed_t speed = pf_boot_config_storage_speed();     /* saved value, or PF_STORAGE_SPEED_DEFAULT */
uint16_t sysclk_mhz = pf_get_sysclk_mhz();           /* configured MHz; 0 keeps the SDK/build clock */
uint8_t storage_mhz = pf_get_storage_speed_mhz();    /* configured 25/50 MHz target, not achieved speed */
pf_boot_config_set_sysclk(PF_SYSCLK_200MHZ);         /* RAM copy only; false for unknown profiles */
pf_boot_config_set_storage_speed(PF_STORAGE_SPEED_HIGH);       /* RAM copy only */
pf_boot_config_save();                               /* task, scheduler running; true after read-back verifies */
```

The header exposes five out-of-line functions and inline configured-MHz getters. `pf_get_sysclk_mhz()` and
`pf_get_storage_speed_mhz()` read the current RAM configuration through the profile getters; neither measures hardware.
The inline `pf_sysclk_profile_mhz()` (`0` for Default) and `pf_storage_speed_mhz()` (`25` or `50`) conversions remain
available for arbitrary profiles, including HID input validation.
The record format, validation and loading stay in `boot_config.c`. The first getter call loads the record
once. Setters change only the RAM copy; saved values take effect at the next boot
because the SD drivers lock their first configuration. The HID handler restores the previous values when a save
is unconfirmed, so reports retain the previous session values. A failed save does not guarantee flash was unchanged:
an SDK safe-zone exit timeout can occur after programming. A later reboot may therefore load the new values despite
the failed command; failed commands do not automatically reboot.

## Host dashboard settings

HID Feature report `2` reads persisted settings as transport-neutral profiles; Feature report `20`, opcode `6`, saves
profiles. The profile payload is two-byte little-endian `system_clock_mhz` followed by one-byte `sdcard_profile`
(`0` Standard, `1` High-Speed). Report `2` also returns the selected board's GPIO configuration. See the exact
offsets and validation rules in the [HID Report Reference](usb/usb-hid-report-reference.md).

The Python, .NET, and WebHID dashboards map these values to select options and verify the saved values returned in
the Feature `20` completion result. **Save and Reboot** sends only opcode `6`; firmware queues its existing deferred
reboot after a successful save. Failed or rejected saves do not reboot. Opcode `2` remains the standalone reboot
command. Reconnection refreshes Feature `2`. The Python/.NET
`GET /api/status` response includes nullable `settings` and `settings_supported` fields. `POST /api/settings` accepts
the system clock and `sdcard_profile`, and returns the values confirmed by the device. Unsupported values return HTTP 400;
unsupported firmware or busy storage returns HTTP 409. Writes are rejected while MSC or storage work owns the media.
The dashboards expose one **SD Card** speed selector. High-Speed is a requested profile; the active negotiated timing
is reported separately because a card or transport may remain at Standard speed. Values apply after reboot and do
not change the active clock or bus immediately.

The version 1 persistent record stores one byte per profile: system clock and shared SD-card speed. It does not
store MHz or separate SPI/SDIO settings. The HID protocol's system clock field still carries MHz.

Boot-applied settings are exposed through the platform facade `platform/boot_config.c/.h`. The USB task is the only
runtime writer. The RAM copy uses the same record type as flash; save refreshes its metadata and CRC. Record
validation and RP2 flash persistence are private static implementation details in `platform/boot_config.c`;
application code uses only the public facade.
Loading reads the fixed record directly, and saving writes one padded flash page; there is no generic byte-store API.
At the start of `main()`, before the scheduler starts, the clock-setting call makes the first getter call, loading
the record through the lock-free early flash read. The SD board setup later reads the same RAM copy when it prepares
the selected transport. Runtime reads and saves belong to the USB task; they are not a general multi-task settings API.

`pf_boot_config_save()` rejects ISR, pre-scheduler and suspended-scheduler callers and re-entrant saves. It skips an identical record and uses `flash_safe_execute` for erase/program (interrupts off and the other
core held, roughly 50 ms; USB can miss frames, so do not save during MSC transfers). Unlike the PicoUart reference,
PicoForge disables FreeRTOS dynamic allocation: the SDK reuses a static lockout task control block. Therefore the
settings module waits for that task to be reclaimed and refuses another save if it is not; it must remain the only
`flash_safe_execute()` caller, and tasks must not be created or deleted during a save or while reclamation is pending.
Reclamation uses an elapsed-tick deadline with a one-tick polling delay; scheduling can delay return beyond the deadline.
An unsafe first access leaves loading pending for a safe caller; setters reject changes until loading is possible.

## Record format

Little-endian, 16 bytes, padded with `0xFF` to one 256-byte page at the start of the reserved sector:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | magic `PFCF` |
| 4 | 2 | format version (`1`) |
| 6 | 2 | record size (`16`) |
| 8 | 1 | System clock profile (`0` Default, `1` 150 MHz, `2` 200 MHz, `3` 250 MHz) |
| 9 | 1 | Shared SD-card speed profile (`0` Default, `1` High-Speed) |
| 10 | 2 | Reserved, must be zero |
| 12 | 4 | CRC-32 (IEEE, reflected) of bytes 0–11 |

Missing, erased, short, wrong magic/version/size, CRC-invalid or out-of-range data returns the complete defaults;
fields are never applied partially and boot never depends on the record. There is a single copy: a power loss during
save leaves a record that fails CRC and loads as defaults (single sector, no wear levelling: fine for infrequent user
configuration; a dual-sector journal is the upgrade if atomic retention or frequent writes are needed).

## Flash reservation

The last 4 KiB sector (`PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE`: 2 MiB on Pico, 4 MiB on Pico 2) is reserved; its
offset is derived from the SDK board definition, not hard-coded. No other project data lives in flash. The `pico_forge`
target shortens only the linker `FLASH` region by one sector through a `pico_add_linker_script_override_path()`
override of `pico_flash_region.ld` (the map shows `0x1ff000` / `0x3ff000`), so an oversized image fails to link.
`PICO_FLASH_SIZE_BYTES` keeps the physical size (`pico_override_flash_size()` is not used because it would change it).
A post-link step ([check_settings_sector.cmake](../../firmware/check_settings_sector.cmake)) additionally compares
`__flash_binary_end` with the reserved sector before the UF2/BIN/HEX outputs are generated; the contract test in
`tests/contracts/test_settings_sector.py` runs it with images ending at, before and after the sector start for both
flash sizes. A board with less flash than the SDK definition is unsupported.

## System clock

A saved system clock profile is applied as the first statement of `main()` in
[main.c](../../firmware/src/main.c), after SDK startup and before monitor UART, application peripherals, USB, and
FreeRTOS start. The FreeRTOS port reads the new `clk_sys` for SysTick at scheduler start.
`pf_system_clock_apply()` ([system.c](../../firmware/src/platform/system.c)) takes no arguments: it reads the saved
setting through `pf_get_sysclk_mhz()` without writing flash, then applies it:

- `0`, a corrupt/missing record (which decodes as Defaults), an underivable frequency or the clock already in use change
  nothing, so Defaults keep the build clock.
- The core voltage is never changed by this call, so profiles above the SDK default clock (including 250 MHz) run at
  whatever voltage the SDK already set.
- The PLL comes from `check_sys_clock_khz()` and `set_sys_clock_pll()`. The SDK moves `clk_peri` to 48 MHz there, which
  would cap SPI at 24 MHz, so `clk_peri` is reconfigured back to `clk_sys` (the SDK boot behavior; a 50 MHz SPI
  ceiling needs at least 100 MHz). On RP2350, `clk_hstx` is reconfigured so its recorded frequency matches. Ticks run
  from `clk_ref` (unchanged), so SDK timer/alarm timing is preserved. Automatic SDK UART/USB stdio initialization
  is disabled; the monitor UART is explicitly initialized after the clock change. Application peripheral baud rates
  and dividers are therefore calculated using the final clock. Keep this call before peripheral initialization.

`SYS_CLK_HZ`, the `main.c` static assertion and the artifact name remain the BUILD clock, not the runtime speed. The
clock selector exposes values, not a guarantee that each value is supported by every board/MCU combination: 150 MHz
on RP2040 and 200/250 MHz are outside the respective MCU specifications, and QSPI flash is clocked from `clk_sys`
so higher clock profiles also affect flash timing. Treat out-of-spec profiles as experimental, not product-qualified.
Do not advertise a profile as supported for a product until that exact board, MCU, flash, clock, and peripheral
combination has been qualified. None of these runtime clock profiles is hardware-qualified by the host model tests.

## Tests

Host tests run with `ceedling test:all`; the focused boot-config check is `ceedling test:test_boot_config`.
They cover defaults, the CRC-32 check value, malformed metadata and profiles, round trips, unchanged-save detection,
failed and partially completed programming, rejected ISR/scheduler contexts, reentrant saves, safe loading retries,
and lockout-TCB reclamation including elapsed-tick wraparound. The SDK fake distinguishes failure before task
creation, failure after task creation but before programming, and failure after programming during safe-zone exit.
Partial-program tests interrupt the record at every byte boundary, check complete defaults after reboot, then verify
recovery through a successful save. Separate system-clock tests exercise modelled clock/regulator APIs.
Real flash erase/program, power loss, clock changes and the bus speeds on hardware are not tested here and need a board.

### Board qualification (pending)

This procedure has not been executed as part of the host checks. Run it on both RP2040 and RP2350 boards before
claiming hardware qualification; use only clock profiles already qualified for the exact board.

1. Record the firmware commit, SDK/FreeRTOS revisions, board/MCU/flash model, transport, build clock, selected profiles,
   and host OS. Use expendable media and disable MSC and storage work before saves.
2. In a temporary diagnostic build, count erase/program callback entries and observe task counts and reclamation
   timeouts without printing inside the flash callback. Keep this module the sole safe-execute caller and keep the
   application task population fixed.
3. From the USB task, alternate two distinct settings for 100 saves without rebooting between saves. Verify each
   result, no stuck reclamation state, and the task count returning to its baseline. Repeat with normal CDC/HID
   traffic active (MSC still disabled), then reboot and verify the final profiles. Use a diagnostic caller for the
   no-reboot loop: the normal successful HID save command intentionally requests reboot.
4. Save identical settings 100 times. Verify every call succeeds without increasing the erase/program counters.
5. With SD writes stopped, interrupt board power at varied points during 20 changing-setting saves. Prevent USB or
   probe back-powering. After each restart, verify boot succeeds and settings are either a valid old/new complete
   record or complete defaults, never an invalid mixed record. Save again to confirm recovery.
6. Record actual counts, timings, USB disconnects/timeouts, reboot results, and any failures. Mark unexecuted or
   interrupted steps pending; host-model passes are not substitutes for these results.
