# Pico 2 SDIO Speed Matrix Repeat

Date: 2026-10-18. Tested firmware commit: `35779bd404d2f8ab5185a912b331449a1a45d191`.

## Conclusion

All six configurations passed three consecutive 64 MiB file write/read-verification runs: **18/18 passed**.
The fastest configuration was **250 MHz CPU / 50 MHz SDIO**, with median throughput of
**16.37 MiB/s write / 15.15 MiB/s read**. This is 8.14% faster writing and 12.26% faster reading than
200 MHz CPU / 50 MHz SDIO in this repeat.

Compared with the [previous matrix](2026-10-18-sdio-speed-matrix.md), median write changes were between
+0.18% and +0.83%; median read changes were between -0.97% and +0.21%. The measurements show no large
throughput change after the architecture refactor, but three runs cannot establish statistical equivalence.

## Hardware and method

- Pico 2 / RP2350, USB serial `575393D4D2C8C043`; probe serial `50543165611F6C9C`.
- Same SB128 card, CID `035344534231323880f3be3cc001411f`, card serial 4089330880.
- Capacity: 249,737,216 sectors / 127,865,454,592 bytes; existing exFAT filesystem.
- Four-bit SDIO-PIO, FreeRTOS application, SD backend, reported firmware version 0.1.0.
- User authorized six verified image flashes and three 64 MiB runs per image.
- Order: CPU150 Default/High Speed, CPU200 Default/High Speed, CPU250 Default/High Speed.
- Fixed flags: fused copy OFF, interrupt wait OFF, file preallocation OFF.
- Each run wrote and read-verified all 67,108,864 bytes and removed `/PFTEST.BIN`.
- Total payload: 1,152 MiB written and 1,152 MiB read, plus filesystem metadata operations.
- No formatting, NOR access or raw-sector test was performed. MSC remained disabled.
- Dashboard HID supplied benchmark control/status; CDC diagnostics were captured before and after each image.
- Throughput is application filesystem MiB/s, not decimal MB/s or raw bus bandwidth.
- Write timing includes pattern generation, writes, sync and close; read timing includes verification and close.
  See the [benchmark procedure](../../sd-self-test.md) for timing and cleanup semantics.

## Results

| CPU MHz | Mode | Actual SD MHz | Passed | Write median (min-max) MiB/s | Read median (min-max) MiB/s |
|---|---|---|---|---|---|
| 150 | Default Speed | 25 | 3/3 | 8.95 (8.86-9.13) | 8.18 (8.18-8.18) |
| 150 | High Speed | 37.5 | 3/3 | 11.43 (11.43-11.47) | 10.41 (10.41-10.41) |
| 200 | Default Speed | 25 | 3/3 | 9.36 (9.36-9.40) | 8.72 (8.70-8.72) |
| 200 | High Speed | 50 | 3/3 | 15.14 (15.04-15.47) | 13.50 (13.48-13.50) |
| 250 | Default Speed | 25 | 3/3 | 9.69 (9.69-9.70) | 9.09 (9.09-9.09) |
| 250 | High Speed | 50 | 3/3 | 16.37 (16.36-16.43) | 15.15 (15.15-15.19) |

CPU150 High Speed is limited to 37.5 MHz by the transport's CPU-derived ceiling. CPU200/250 negotiated 50 MHz.
No image fell back to Default Speed.

## Individual runs

All runs completed with zero mismatches, benchmark errors, FatFs errors and request errors; no test file remained.
The volume remained mounted.

| CPU / SD MHz | Run | Write MiB/s | Read MiB/s |
|---|---|---|---|
| 150 / 25 | 1 | 9.13 | 8.18 |
| 150 / 25 | 2 | 8.95 | 8.18 |
| 150 / 25 | 3 | 8.86 | 8.18 |
| 150 / 37.5 | 1 | 11.43 | 10.41 |
| 150 / 37.5 | 2 | 11.43 | 10.41 |
| 150 / 37.5 | 3 | 11.47 | 10.41 |
| 200 / 25 | 1 | 9.36 | 8.72 |
| 200 / 25 | 2 | 9.36 | 8.72 |
| 200 / 25 | 3 | 9.40 | 8.70 |
| 200 / 50 | 1 | 15.47 | 13.48 |
| 200 / 50 | 2 | 15.14 | 13.50 |
| 200 / 50 | 3 | 15.04 | 13.50 |
| 250 / 25 | 1 | 9.70 | 9.09 |
| 250 / 25 | 2 | 9.69 | 9.09 |
| 250 / 25 | 3 | 9.69 | 9.09 |
| 250 / 50 | 1 | 16.36 | 15.15 |
| 250 / 50 | 2 | 16.37 | 15.19 |
| 250 / 50 | 3 | 16.43 | 15.15 |

## Diagnostics and final state

After each image's three runs, diagnostics reported one initialization attempt, zero retries, zero High-Speed
fallbacks, `error=none` and `high_speed_error=none`. Command/data CRC errors, command/data/DMA/busy/PIO timeouts,
TX/RX stalls, batch fallbacks and retained failure sequence were all zero.
Recovery counters remain `reserved`, not measured zero.

The board was left running the tested commit at **250 MHz CPU / 50 MHz SDIO**, with exFAT mounted,
MSC disabled, benchmark inactive/passed, and no benchmark file remaining.

## Image identity and evidence

Images were built under `build/sdio-matrix-cpu<MHz>-ds` and `build/sdio-matrix-cpu<MHz>-hs`.
These build directories now contain this repeat's images, not the previous matrix's images.

| Image | ELF SHA-256 |
|---|---|
| cpu150-ds | `406790affc5757d3f8d89ed695dd56458785be40fbde4691c28359e11aa4159b` |
| cpu150-hs | `b50df7b45d76f600da845d2dbed58d50c098981c9b83c3b59e16e6ba88e38a95` |
| cpu200-ds | `862e0e4d004eaca7999c62cab492a4af6ca5c954c0358c5d972ab56278c3e748` |
| cpu200-hs | `b67a6a973489588813ee68b15ca4ff4300c90a5f180b907dcd850143e760c889` |
| cpu250-ds | `61c9de1590aaf326d0032e3fdecb920baa1ebb91636de711c757db3bb45a061b` |
| cpu250-hs | `ab6fb91c22b6c8f6d535a699f28e3257087a5ef5f98cdb477112d4a0682bfcf7` |

Raw results, per-run JSON snapshots, before/after CDC diagnostics and configure/build/flash logs are retained in
the Copilot session's `files/sdio-matrix-repeat-35779bd/`; the runner is `files/test_sdio_matrix_repeat.py`.
The earlier matrix evidence remains separate.

## Limits

One board/card, fixed test order and no power cycle between images. Filesystem allocation history and runtime
conditions can affect results. CPU200/250 are overclocks, not vendor-rated operating-point qualification.
These bounded checks do not establish endurance, thermal/supply margins, all-card compatibility, power-loss
safety, full-capacity authenticity or USB MSC interoperability. Earlier failures remain valid historical
observations even though they did not recur here. No firmware defaults were changed.
