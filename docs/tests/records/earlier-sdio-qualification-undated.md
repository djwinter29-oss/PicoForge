# Earlier SDIO Qualification Observations (Undated)

Date: not recorded. Migrated unchanged from the former "Previously Recorded Qualification" section of
[SD-card driver design](../../design/sdcard/sdcard-driver-design.md); the observations pre-date the 2026-10-13 records.

**Scope:** historical, configuration-specific observations. Earlier clocking, recovery, and qualification algorithms
may differ from the current implementation, and these records do not qualify every compatible card or the current
firmware. Later controlled comparisons are the [2026-10-18 speed matrix](2026-10-18-sdio-speed-matrix.md) and its
[repeat measurement](2026-10-18-sdio-speed-matrix-repeat.md).

| Card/configuration | Recorded result |
| --- | --- |
| 8 GB Class 10, 250 MHz SYSCLK | 32 MiB self-test exercised lower-clock CMD12 retry, completed near 33 MHz with zero mismatches; 8.48 MB/s write and 9.91 MB/s read |
| 16 GB Class 4, 20 MHz request / 18.75 MHz actual | 32 MiB passed; 7.11 MB/s write and 6.69 MB/s read |
| 8 GB SD8GB, stock 150 MHz SYSCLK / 25 MHz SDIO | 32 MiB passed with zero mismatches; 7.22 MB/s write and 1.45 MB/s read |
| 128 GB SN128, 250 MHz SYSCLK / 50 MHz SDIO | 32 and 128 MiB self-tests recorded zero mismatches; 15.9 MB/s write and 13.7 MB/s read |
| 128 GB SN128, 200 MHz SYSCLK / 50 MHz SDIO | Recorded 12.5 MB/s write and 9.5 MB/s read |
| 128 GB SN128, stock 150 MHz SYSCLK / 25 MHz SDIO | Recorded 9.5 MB/s write and 8.3 MB/s read |
| 128 GB SN128, 280 MHz SYSCLK | Did not work with the card even at 25 MHz SDIO |

## Receive and transmit configuration used

Those experiments used falling-edge RX with a fast data divider half the command/clock divider. The current RX phases
and system-clock sampling are documented in the
[physical transport reference](../../../firmware/src/drivers/storage/README.md#physical-storage-bus).
High-speed TX starts the clock and data state machines together and launches at a fixed cycle offset. The slow TX
program is not suitable for 50 MHz: its observed-edge launch left insufficient setup time and produced card CRC-error
tokens. A 50 MHz bus needs at least 200 MHz SYSCLK; those Pico 2 overclock experiments are diagnostic, not supported
production clock settings. These are historical experiments; the default driver targets 25 MHz. High-speed PIO programs
are selected only after the explicit build opt-in and a confirmed CMD6 switch. Both former initialization scans and the
later 128 KiB probe have been replaced by a single-sector validation at each startup timing.

## Pico 2 dual-core MSC handoff observation

Migrated from [architecture](../../design/architecture.md). On the connected Pico 2 at 250 MHz CPU / 50 MHz SDIO, probe
inspection confirmed the active and passive idle tasks executing on separate cores. The dashboard enable command
completed worker-owned unmount and re-enumerated the SD card as a 119.1 GiB USB block device with its existing
partition. Host handoff tests pass, but sustained dual-core stress remains unqualified.
