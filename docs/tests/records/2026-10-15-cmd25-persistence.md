# Cross-call CMD25 persistence A/B benchmark

## Decision

Keep cross-call native CMD25 persistence unconditionally. The temporary A/B
build option was removed after the decision. On the connected card, persistence
improved median filesystem write throughput by **40.93%**, well above the
5-10% simplification threshold. Closing each facade call increased median write
time from 16,478 ms to 23,222 ms. Persistence reduced elapsed write time by
29.04%; throughput gain and time reduction use different denominators.

This result does not justify removing `write_stream_next_sector` or the
cross-call stream state. It qualifies this card and workload, not all cards or
power-loss/removal behavior.

## Controlled setup

- Date: 2026-10-15; Pico 2 / RP2350, USB serial `575393D4D2C8C043`.
- Same physical SD8GB card throughout: manufacturer ID 65, OEM `42`, revision
  3.0, card serial 5742642, manufactured February 2015; 15,564,800 sectors,
  7,969,177,600 bytes.
- Existing exFAT volume, unchanged and never formatted.
- Stock 150 MHz SYSCLK and native 4-bit SDIO at 25 MHz, checked through HID
  before and after every run. No clock fallback occurred.
- Release builds, fused copy OFF, interrupt waits OFF, and
  `PF_STORAGE_SELF_TEST_PREALLOCATE=ON` in both variants.
- Each run used a new 128 MiB `/PFTEST.BIN`, contiguous allocation before write
  timing, and the unchanged 32 KiB self-test buffer and pattern.
- The only benchmark behavior difference was the temporary A/B compile-time
  switch: the persistent variant retained sequential CMD25 across facade calls;
  the closed variant performed checked stream closure before successful return.
- Profiling was enabled and reset before every run in both variants. No other
  monitor was active; MSC was disabled. Each artifact was programmed and
  verified using the same CMSIS-DAP probe at 1 MHz. The filesystem was cleanly
  unmounted before each flash and confirmed mounted before testing.
- Order: persistent, closed, closed, persistent, persistent, closed. Each
  measurement followed a fresh flash/reset, including consecutive equal
  variants. Allocation and cleanup were outside the write timing; final sync
  and close were inside it. Full readback verification was unchanged.

The historical build parameters and repetition procedure are recorded above;
the temporary switch is no longer available in production builds. Source
baseline: `1b78abe46ec3429e03060f8b3839e965da2a5f6c` plus the then-uncommitted
A/B switch, routing checks, and build/documentation changes.

ELF SHA-256:

| Variant | SHA-256 |
| --- | --- |
| Persistent | `1fbdc7f2fa4f7e04230ecba1966803a59c0dec295685bde4a0e0ad674ab575b9` |
| Closed per call | `8625d77809ace22c066424089676f46bd9e71fa728d0d390e21db4ba17e47c25` |

## Measurements

Every row wrote and read exactly 134,217,728 bytes, passed with zero mismatches,
reported no request/filesystem/test errors, removed its temporary file, and
left the volume mounted. All four profile event counters (CRC failures, batch
fallbacks, timeouts, recovery attempts) were zero in every run.

| Order | Variant | Write ms | Read/verify ms | Write MiB/s |
| --- | --- | ---: | ---: | ---: |
| 1 | Persistent | 16,704 | 17,736 | 7.663 |
| 2 | Closed per call | 23,421 | 17,739 | 5.465 |
| 3 | Closed per call | 23,222 | 17,736 | 5.512 |
| 4 | Persistent | 16,478 | 17,725 | 7.768 |
| 5 | Persistent | 16,478 | 17,722 | 7.768 |
| 6 | Closed per call | 23,192 | 17,739 | 5.519 |
| Median | Persistent | **16,478** | **17,725** | **7.768** |
| Median | Closed per call | **23,222** | **17,739** | **5.512** |

Comparator invocation:

```sh
python3 tools/validation/compare-cmd25-benchmark.py \
  --persistent-ms 16704 16478 16478 \
  --closed-ms 23421 23222 23192
```

Output: persistent throughput gain 40.93%; relative median absolute deviation
0.00% persistent and 0.13% closed; decision retain cross-call persistence.
The zero persistent median deviation reflects two equal millisecond samples,
not absence of variability: its full write-time range was 16,478-16,704 ms.

Raw terminal reports, decoded HID status, exact results, timestamps, and
artifact hashes are retained in
[the JSON record](2026-10-15-cmd25-persistence.json).

## Profile evidence

Whole-test snapshots include preparation, writing, reading, and cleanup.
Persistent runs recorded 8,274 command calls and 4,097 stop calls; closed runs
recorded 16,464 command calls and 8,192 stop calls. Closing per call therefore
added 4,095 stops, consistent with restarting at the 32 KiB facade boundary.
Persistent stop time was 165,224-165,665 us versus 5,508,369-5,528,711 us closed.
This supports a material stop/card-busy cost rather than a small command-only
overhead. Profile windows overlap, especially stop and busy; do not sum them or
treat the whole-test counters as write-only timings.

## Validation and final device state

- Both Pico 2 firmware builds completed.
- `check-sdio-init.py` passed in both persistence modes.
- `check-sdio-tx.py` passed CMD25 continuation/recovery and TX regressions.
- Comparator synthetic checks covered the below-5%, 5-10%, and at-least-10%
  decisions, non-positive times, fewer than three runs, and unequal counts.
- `sh -n tools/firmware/build.sh` passed.
- Six hardware runs passed the checks above.
- The persistent benchmark ELF was restored and flash verification passed.
  Final HID status confirmed the same card, 150 MHz SYSCLK, 25 MHz SDIO,
  MSC disabled, and no active self-test. This artifact retains benchmark
  preallocation ON; the production preallocation default was not changed.

The temporary A/B switch was subsequently removed and persistence made
unconditional. No persistence state was removed. Host checks and this short HIL
benchmark do not qualify endurance, arbitrary card models, media removal during
writes, or power-loss recovery.
