# Persistent settings

Settings live in one reserved flash sector. Each image has a compile-time transport (Pico/Pico 2 × SPI/SDIO_PIO);
there is no runtime transport selector.

| Field | Values | Default (`0`) | Used by |
| --- | --- | --- | --- |
| `system_clock_mhz` | 0, 150, 200, 250 | Keep the build clock (`PF_SYS_CLK_MHZ`, else the SDK stock clock) | Both; applied at boot |
| `sdio_bus_mhz` | 0, 25, 50 | 25 MHz Default Speed only | SDIO_PIO images |
| `spi_bus_mhz` | 0, 25, 50 | 50 MHz ceiling (board profile) | SPI images |

An image reads only its own bus-speed field. Values are ceilings: SPI above 25 MHz needs the card's CMD6 High-Speed
support, otherwise the existing CSD-limited 25 MHz is kept. For SDIO, `0` and `25` stay at 25 MHz Default Speed; `50`
allows the existing High-Speed negotiation, still limited by `PF_SDIO_PIO_HIGH_SPEED`, SYSCLK/4 and the card. SDIO
remains limited to the fixed 25 MHz transfer mode plus that optional negotiation.

## Using the API

```c
#include "drivers/persistent_memory/settings_store.h"

pf_settings_t settings;
bool stored = pf_settings_load(&settings);   /* task, scheduler running; always complete (defaults if !stored) */
settings.spi_bus_mhz = 25;
pf_settings_save(&settings);                 /* task, scheduler running; true after read-back verifies */
```

## Host dashboard settings

HID Feature report `2` reads persisted settings as transport-neutral profiles; Feature report `20`, opcode `6`, saves
profiles. The profile payload is two-byte little-endian `system_clock_mhz` followed by one-byte `sdcard_profile`
(`0` Standard, `1` High-Speed). Report `2` also returns the selected board's GPIO configuration. See the exact
offsets and validation rules in the [HID Report Reference](usb/hid-report-reference.md).

The Python, .NET, and WebHID dashboards map these values to select options and verify a save by reading Feature
report `2` back. Saving does not reboot. **Save & Reboot** issues a separate confirmed reboot command through Feature
report `20`, opcode `2`; firmware waits one second before resetting. The Python/.NET
`GET /api/status` response includes nullable `settings` and `settings_supported` fields. `POST /api/settings` accepts
the system clock and `sdcard_profile`, and returns the values confirmed by the device. Unsupported values return HTTP 400;
unsupported firmware or busy storage returns HTTP 409. Writes are rejected while MSC or storage work owns the media.
The dashboards expose one **SD Card** speed selector. High-Speed is a requested profile; the active negotiated timing
is reported separately because a card or transport may remain at Standard speed. Values apply after reboot and do
not change the active clock or bus immediately.

For compatibility, the persistent settings record remains unchanged. The firmware maps the build-selected transport's
legacy numeric field to the profile in Feature report `2`, and maps a saved profile back to that field. SDIO `0`/`25`
maps to Standard and `50` to High-Speed; SPI `25` maps to Standard and `0`/`50` to High-Speed. A missing record retains
the existing transport defaults.

Settings follow PicoUart's small boot-config pattern: one settings module owns the record and the only runtime writer
is the USB task ([reference implementation](https://github.com/djwinter29-oss/PicoUart/blob/main/firmware/src/platform/boot_config.c)).
PicoForge keeps its portable record codec separate because the HID/profile migration and protocol tests share it.
Before the scheduler starts, runtime-init and `main()` use `pf_settings_load_early()`; `main()` passes the result to
`pf_board_apply_settings()` before storage starts. Runtime reads and saves belong to the USB task; they are not a
general multi-task settings API.

`pf_settings_save()` rejects ISR, pre-scheduler and suspended-scheduler callers, invalid settings, and re-entrant
saves. It skips an identical record and uses `flash_safe_execute` for erase/program (interrupts off and the other
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
| 10 | 1 | `sdio_bus_mhz` |
| 11 | 1 | `spi_bus_mhz` |
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

A saved `system_clock_mhz` is applied by a `PICO_RUNTIME_INIT_FUNC_HW` hook in [main.c](../../firmware/src/main.c) ordered
`00501`: after the SDK clock init (`00500`) and before peripheral resets are released (`00600`), stdio and FreeRTOS, so
the FreeRTOS port reads the new `clk_sys` for SysTick at scheduler start. The hook reads the record without writing
(`pf_settings_load_early()`) and calls `pf_system_clock_apply()` ([system.c](../../firmware/src/drivers/system.c)):

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

Host tests (`firmware/tests/config`, `test_sd_native_init.c`) run with `ceedling test:all`; they cover defaults, the
CRC-32 check value, every-bit CRC coverage, each corruption class, the flash store against a host flash model (round
trip, no redundant erase, failed program, rejected ISR/scheduler contexts, re-entrant and overlapping load/save through
a modelled mutex, lockout-TCB reclamation) and the clock sequence against modelled clock/regulator APIs.
Real flash erase/program, power loss, clock changes and the bus speeds on hardware are not tested here and need a board.
