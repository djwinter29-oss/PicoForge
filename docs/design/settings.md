# Persistent settings

Settings live in one reserved flash sector. Each image has a compile-time transport (Pico/Pico 2 × SPI/SDIO_PIO);
there is no runtime transport selector.

| Setting | Values | Default | Applied |
| --- | --- | --- | --- |
| System clock (`pf_sysclk_profile_t`) | `PF_SYSCLK_DEFAULT`, `PF_SYSCLK_150MHZ`, `PF_SYSCLK_200MHZ`, `PF_SYSCLK_250MHZ` | Default: keep the build clock (`PF_SYS_CLK_MHZ`, else the SDK stock clock) | At boot |
| Storage speed (`pf_storage_speed_t`) | `PF_STORAGE_SPEED_DEFAULT` (25 MHz), `PF_STORAGE_SPEED_HIGH` (50 MHz) | Default (25 MHz) | At boot |

Both SD transports obtain their requested clock from
`pf_get_storage_speed_mhz()` in
[sd_board.c](../../firmware/src/drivers/sdcard/sd_board.c). The 25/50 MHz value is a target, not a guaranteed rate:
the hardware chooses the closest safe supported clock at or below it. Board data supplies wiring and controller
selection, not a separate speed. SPI above 25 MHz requires card CMD6 High-Speed support; otherwise the CSD-limited
Default-Speed rate is retained. SDIO High-Speed also depends on `PF_SDIO_PIO_HIGH_SPEED`, SYSCLK/4, and the card.
For example, a 50 MHz SDIO request at 150 MHz SYSCLK achieves 37.5 MHz. Status reports the actual bus clock separately.

## Using the API

```c
#include "drivers/boot_config.h"

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
available for arbitrary profiles, including record encoding and HID input validation.
The record format, validation and loading stay in `boot_config.c`. The first getter call loads the record
once. Setters change only the RAM copy; saved values take effect at the next boot
because the SD drivers lock their first configuration. The HID handler restores the previous values when a save
does not complete, so reports never show unsaved values.

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

For compatibility, the persistent settings record remains unchanged. Saving writes the profile as `25`/`50` to both
legacy bus bytes; loading reads High-Speed when either byte is `50` and Standard otherwise, so `boot_config` does not
depend on the build transport. Older firmware wrote only its own transport's byte and left the other `0`, which
this rule still reads correctly. A missing record uses Default.

Boot-applied settings live behind the driver `drivers/boot_config.c/.h`. The USB task is the only runtime writer.
RP2 flash access and FreeRTOS-safe erase/program handling are isolated in the platform store
`platform/boot_config_store.c/.h`; the boot-config driver owns the record format, validation, defaults, and public
API. This keeps hardware-specific flash operations behind the platform interface.
Before the scheduler starts, the runtime-init hook makes the first getter call, so the record is loaded through the
lock-free early flash read. The SD board setup later reads the same RAM copy when it prepares the selected transport. Runtime reads and saves belong to the USB task; they
are not a general multi-task settings API.

`pf_boot_config_save()` rejects ISR, pre-scheduler and suspended-scheduler callers and re-entrant saves. It skips an identical record and uses `flash_safe_execute` for erase/program (interrupts off and the other
core held, roughly 50 ms; USB can miss frames, so do not save during MSC transfers). Unlike the PicoUart reference,
PicoForge disables FreeRTOS dynamic allocation: the SDK reuses a static lockout task control block. Therefore the
settings module waits for that task to be reclaimed and refuses another save if it is not; it must remain the only
`flash_safe_execute()` caller, and tasks must not be created or deleted during a save.

## Record format

Little-endian, 16 bytes, padded with `0xFF` to one 256-byte page at the start of the reserved sector:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | magic `PFCF` |
| 4 | 2 | format version (`1`) |
| 6 | 2 | record size (`16`) |
| 8 | 2 | `system_clock_mhz` |
| 10 | 1 | SDIO bus MHz (`0`/`25` Default, `50` High) |
| 11 | 1 | SPI bus MHz (`0`/`25` Default, `50` High) |
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

A saved system clock profile is applied by a `PICO_RUNTIME_INIT_FUNC_HW` hook in [main.c](../../firmware/src/main.c) ordered
`00501`: after the SDK clock init (`00500`) and before peripheral resets are released (`00600`), stdio and FreeRTOS, so
the FreeRTOS port reads the new `clk_sys` for SysTick at scheduler start. The hook reads the record without writing
(through `pf_get_sysclk_mhz()`) and calls `pf_system_clock_apply()` ([system.c](../../firmware/src/drivers/system.c)):

- `0`, a corrupt/missing record (which decodes as Defaults), an underivable frequency or the clock already in use change
  nothing, so Defaults keep the build clock.
- Voltage is raised only when the target is above the current clock and the regulator is below the need (1.15 V up to
  200 MHz, 1.30 V above), settled with `busy_wait_at_least_cycles()` for the SDK's 1000 µs (TIMER is still in reset).
  It is never lowered, so a compile-time overclock keeps its voltage.
- The PLL comes from `check_sys_clock_khz()` and `set_sys_clock_pll()`. The SDK moves `clk_peri` to 48 MHz there, which
  would cap SPI at 24 MHz, so `clk_peri` is reconfigured back to `clk_sys` (the SDK boot behavior; a 50 MHz SPI
  ceiling needs at least 100 MHz). On RP2350, `clk_hstx` is reconfigured so its recorded frequency matches. Ticks run
  from `clk_ref` (unchanged) and peripherals are still in reset, so nothing else sees the transition.

`SYS_CLK_HZ`, the `main.c` static assertion and the artifact name remain the BUILD clock, not the runtime speed. The
clock selector exposes values, not a guarantee that each value is supported by every board/MCU combination: 150 MHz
on RP2040 and 200/250 MHz are outside the respective MCU specifications, and QSPI flash is clocked from `clk_sys`
(the same exposure as a compile-time overclock). Treat out-of-spec profiles as experimental, not product-qualified.
Do not advertise a profile as supported for a product until that exact board, MCU, flash, clock, and peripheral
combination has been qualified. None of these runtime clock profiles is hardware-qualified by the host model tests.

## Tests

Host tests (`firmware/tests/drivers`, `test_sd_native_init.c`) run with `ceedling test:all`; they cover defaults, the
CRC-32 check value, every-bit CRC coverage, each corruption class, the flash store against a host flash model (round
trip, no redundant erase, failed program, rejected ISR/scheduler contexts, re-entrant and overlapping load/save through
a modelled mutex, lockout-TCB reclamation) and the clock sequence against modelled clock/regulator APIs.
Real flash erase/program, power loss, clock changes and the bus speeds on hardware are not tested here and need a board.
