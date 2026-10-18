# Pico 2 SDIO Speed Matrix

Date: 2026-10-18. Firmware commit: `9e569e4c379077482318165cb76332f92e91fd09`.

## Conclusion

All six configurations passed three consecutive 64 MiB file write/read-verification runs: **18/18 passed**.
The fastest measured configuration was **250 MHz CPU / 50 MHz SDIO**, with median filesystem throughput of
**16.34 MiB/s write / 15.30 MiB/s read**. Compared with 200 MHz CPU / 50 MHz SDIO, this was 8.81% faster writing
and 13.60% faster reading.

For a stock CPU clock, 150 MHz CPU / negotiated 37.5 MHz SDIO passed and delivered 11.41 / 10.41 MiB/s.
200 MHz CPU / 50 MHz SDIO is an intermediate performance option, but both 200 and 250 MHz are overclock settings;
these bounded passes do not establish vendor-rated operation or production reliability.

Earlier records describe failures at 250 MHz CPU / 50 MHz SDIO on earlier firmware. That failure did not recur
in these three runs on the recorded commit. This does not retrospectively invalidate the earlier observations
or prove the underlying hardware margins.

## Hardware and method

- Pico 2 / RP2350, USB serial `575393D4D2C8C043`.
- Raspberry Pi Debug Probe serial `50543165611F6C9C`.
- SD card: SB128, manufacturer 3, OEM SD, revision 8.0, manufacture date 2020-01.
- CID: `035344534231323880f3be3cc001411f`; card serial 4089330880.
- Capacity: 249,737,216 sectors, 127,865,454,592 bytes; existing exFAT filesystem.
- Four-bit SDIO-PIO, FreeRTOS application, SD storage backend, reported firmware version 0.1.0.
- Fixed flags: `PF_SDIO_PIO_FUSED_COPY=OFF`, `PF_SD_CARD_INTERRUPT_WAIT=OFF`,
  `PF_STORAGE_SELF_TEST_PREALLOCATE=OFF`. Allocation was normal FatFs allocation, not preallocated.
- Six images were built and probe-flashed with verification. Each image was allowed to initialize and automount.
- Execution order: CPU150 Default/High Speed, CPU200 Default/High Speed, CPU250 Default/High Speed.
- Each image ran three sequential 64 MiB benchmarks. All 67,108,864 bytes were written and verified in each run.
- Total benchmark payload: 1,152 MiB written and 1,152 MiB read; filesystem metadata adds further I/O.
- Only the reserved `/PFTEST.BIN` was intentionally replaced and deleted. No formatting or raw-sector test ran.
- MSC remained disabled. The existing dashboard supplied HID status and benchmark control;
  CDC `sd info` and `sd diagnostics` were collected before and after each configuration.
- Speeds are **MiB/s**, with 1 MiB = 1,048,576 bytes. They are not decimal MB/s or raw bus bandwidth.
- Write timing includes pattern generation, filesystem writes, final sync and close.
  Read timing includes reads, verification and close. Preparation, file open and cleanup are outside phase timings.

The High-Speed build option requests up to 50 MHz and respects the transport's CPU-derived ceiling.
At CPU150 this produced 37.5 MHz; at CPU200/250 it produced 50 MHz. No configuration fell back to Default Speed.

## Summary

| CPU MHz | Mode | Actual SD MHz | Passed | Write median (min-max) MiB/s | Read median (min-max) MiB/s |
|---|---|---|---|---|---|
| 150 | Default Speed | 25 | 3/3 | 8.89 (8.86-9.07) | 8.24 (8.24-8.26) |
| 150 | High Speed | 37.5 | 3/3 | 11.41 (11.40-11.41) | 10.41 (10.40-10.41) |
| 200 | Default Speed | 25 | 3/3 | 9.34 (9.33-9.39) | 8.79 (8.78-8.79) |
| 200 | High Speed | 50 | 3/3 | 15.02 (14.98-15.55) | 13.47 (13.45-13.50) |
| 250 | Default Speed | 25 | 3/3 | 9.67 (9.60-9.71) | 9.15 (9.14-9.15) |
| 250 | High Speed | 50 | 3/3 | 16.34 (16.08-16.35) | 15.30 (15.29-15.30) |

## Individual runs

Every run passed with zero mismatches, benchmark errors, FatFs errors and request errors.
Each finished with the test file removed and the volume still mounted.

| CPU / SD MHz | Run | Write MiB/s | Read MiB/s |
|---|---|---|---|
| 150 / 25 | 1 | 8.89 | 8.24 |
| 150 / 25 | 2 | 8.86 | 8.24 |
| 150 / 25 | 3 | 9.07 | 8.26 |
| 150 / 37.5 | 1 | 11.41 | 10.40 |
| 150 / 37.5 | 2 | 11.41 | 10.41 |
| 150 / 37.5 | 3 | 11.40 | 10.41 |
| 200 / 25 | 1 | 9.34 | 8.79 |
| 200 / 25 | 2 | 9.33 | 8.79 |
| 200 / 25 | 3 | 9.39 | 8.78 |
| 200 / 50 | 1 | 15.02 | 13.47 |
| 200 / 50 | 2 | 14.98 | 13.45 |
| 200 / 50 | 3 | 15.55 | 13.50 |
| 250 / 25 | 1 | 9.67 | 9.14 |
| 250 / 25 | 2 | 9.71 | 9.15 |
| 250 / 25 | 3 | 9.60 | 9.15 |
| 250 / 50 | 1 | 16.34 | 15.30 |
| 250 / 50 | 2 | 16.08 | 15.30 |
| 250 / 50 | 3 | 16.35 | 15.29 |

## Diagnostics and final device state

After all three runs on each image, `sd info` reported:

- `error=none`, `high_speed_error=none`, one initialization attempt, zero retries and zero 50-to-25 MHz fallbacks.
- Zero command/data CRC errors, command/data/DMA/busy/PIO timeouts, TX stalls and RX stalls.
- `sd diagnostics`: result `ok`, zero CRC failures, batch fallbacks and timeouts.
- Recovery counters labeled `reserved` are not implemented measurements and are not claimed to be zero.

The board was left running **250 MHz CPU / 50 MHz SDIO**, firmware from the recorded commit, exFAT mounted,
MSC disabled, benchmark inactive and passed, with no benchmark file remaining.

## Image identity and reproduction

Each image lives under `build/sdio-matrix-cpu<MHz>-ds` or `build/sdio-matrix-cpu<MHz>-hs`.
Use `PF_SYS_CLK_MHZ=150|200|250` and `PF_SDIO_PIO_HIGH_SPEED=OFF|ON`, respectively, with the fixed flags above.
The [file benchmark procedure](../../sd-self-test.md) describes host control, cleanup and timing boundaries.

| Image | ELF SHA-256 |
|---|---|
| cpu150-ds | `84cdbdab2ff88b026a280a34b79fe9f5e190cd8699b25e4046a462ef23b35ca0` |
| cpu150-hs | `f73b41fc414ca0012a11786c38d383780b5c3c0b3b3574be1b42b62497b511b5` |
| cpu200-ds | `cffc07fe8b6a8bd848fc787ff34e023e4f737d3a263da3453ea95542402f1f98` |
| cpu200-hs | `a2a8e1ba9e975d4d7ac49cd266376de933683d36c0d47edb7beeb46dd90231d3` |
| cpu250-ds | `f157d6d3a44d2312dabe4a4d775d88aa9811b45bf42f579660471c3097ac7136` |
| cpu250-hs | `ee254ea41d4ef1aa48ec764ce56da81bd7ac69c965829c13e4f32452ebe471d2` |

Raw JSON snapshots, before/after CDC diagnostics, build/flash logs and the matrix runner are retained in the
Copilot session's `files/sdio-matrix/` and `files/test_sdio_matrix.py`; they are not committed repository fixtures.

## Limits and next qualification

The order was fixed, not randomized, and the same card was reused without power cycling between runs.
Card allocation/cache history and runtime conditions can influence results; these measurements isolate build flags,
not every environmental variable.

Three runs per image on one board/card do not establish long-duration stability, hot/cold or supply margins,
multi-card compatibility, power-loss safety, wear, USB MSC interoperability, or full-capacity authenticity.
Before adopting an overclocked default, run longer randomized tests, repeated cold starts, and controlled
temperature/supply qualification. No firmware default was changed by this experiment.
