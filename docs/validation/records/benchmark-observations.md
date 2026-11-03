# Historical Benchmark Observations

Extracted from the benchmark guide. These observations retain their historical configuration and cleanup behavior;
current operating instructions remain in the [benchmark guide](../../guides/sdcard-benchmark.md).

## Historical Measurements

The observations below describe earlier firmware and test workflows, including older buffer sizes and unmount
behavior. They do not override the current [running safeguards](../../guides/sdcard-benchmark.md#running) or [measurement boundaries](../../guides/sdcard-benchmark.md#measurement).
For later card/clock comparisons, see the
[2026-10-18 speed matrix](2026-10-18-sdio-speed-matrix.md) and its
[repeat measurement](2026-10-18-sdio-speed-matrix-repeat.md).

On 2026-10-13, the same connected Pico 2 and 16 GB card passed the 32 MiB test
before batching, after CMD25 writes, and after CMD18 reads were added, in SDIO
mode at 12.5 MHz through the Python dashboard in Chromium:

| Metric | Single-block baseline | CMD25 writes | CMD25 + CMD18 |
| --- | --- | --- | --- |
| Written and verified | 33,554,432 bytes each | 33,554,432 bytes each | 33,554,432 bytes each |
| Write time | 391,152 ms | 34,734 ms | 35,140 ms |
| Read/verify time | 60,402 ms | 59,332 ms | 22,354 ms |
| Average write | 0.082 MiB/s | 0.922 MiB/s | 0.911 MiB/s |
| Average read | 0.530 MiB/s | 0.539 MiB/s | 1.432 MiB/s |
| Mismatched bytes | 0 | 0 | 0 |
| Cleanup | File removed, volume unmounted | File removed, volume unmounted | File removed, volume unmounted |

Further same-card 32 MiB runs at 12.5 MHz tested two alternating write wire
buffers, preparing the next sector during the current DMA/programming wait.
With the original CRC arithmetic, writing took 41,023 ms (0.780 MiB/s) and
read/verify took 22,363 ms (1.431 MiB/s). Replacing the CRC polynomial
multiplication with equivalent shifted XOR terms reduced these to 36,080 ms
(0.887 MiB/s) and 18,456 ms (1.734 MiB/s). Both runs passed with zero mismatches,
file removal, and volume unmount. These single-run comparisons are indicative,
not guaranteed gains; card latency varies. The CRC optimization is also checked
against an independent bitwise per-lane reference in the host tests.

The next write bottleneck was stream restarts, not the bus. On the same card,
raw CMD25 bursts into fresh 8 MiB-aligned space ran at 0.81 MiB/s (8 KiB per
burst), 3.81 MiB/s (32 KiB) and 4.71 MiB/s (128 KiB). FatFs issues one cluster
(here 32 KiB) per `disk_write`, so the SDIO driver now keeps the CMD25 stream
open across writes to consecutive sectors and ends it with CMD12 on any other
access or sync. With that, the unchanged 8 KiB-write 32 MiB test at 150 MHz
wrote in 7,165 ms (4.47 MiB/s, up from 0.89) and read/verified in 15,371 ms
(2.08 MiB/s), with zero mismatches, file removal and volume unmount. The test
also fills and checks its pattern a word at a time, which removed CPU time from
both phases.

In a historical Pico 2 build using the then-supported 200 MHz build-clock override, with the SD bus unchanged at
12.5 MHz,
the same 32 MiB test passed with zero mismatches: write 34,972 ms (0.915 MiB/s)
and read/verify 15,904 ms (2.012 MiB/s). Reads gained about 16% from the faster
core; writes did not, because they were limited by CMD25 stream restarts (see below), not CPU.

Increasing the SDIO CMD18 batch from 16 to 32 sectors and the self-test read
buffer from 8 KiB to 16 KiB raised a stock-clock 32 MiB read/verify run to
14,134 ms (2.26 MiB/s), up from 2.03 MiB/s in the preceding run. It passed with
zero mismatches and removed the file. An earlier 64-sector attempt timed out
and used `set y` with an unsupported 63 operand (PIO SET has a 5-bit immediate).
A 16.7 MHz SDIO profile failed its first write and is not retained.

The read batch is now 64 sectors (32 KiB, matching the self-test buffer).
The count is loaded through the TX FIFO (`pull`, `mov y, osr`) before the
nibble count. Completed blocks are derived from DMA `write_addr`, checked to
lie word-aligned within the wire buffer. After the CMD18 response is validated,
decoding and CRC checks overlap DMA reception of later blocks in the same
buffer. A CRC failure stops decoding, but the transfer still drains (subject
to its timeout) and CMD12 is attempted before returning an error.

CRC byte lookup and byte lane mapping reduce CPU work without removing checks.
Additional static storage is about 34.5 KiB: 16 KiB self-test buffer growth,
16.25 KiB wire buffer growth, 2 KiB CRC table and 256 bytes for lane mapping.
Final Pico 2 hardware validation at 150 MHz CPU / 12.5 MHz SDIO passed three
128 MiB write/read/verify runs: read 4.693–4.696 MB/s, write 5.032–5.169 MB/s,
zero mismatches and successful cleanup. These are short filesystem benchmarks,
not a continuous acquisition guarantee. See
[the read-throughput record](2026-10-13-sdio-read-throughput.md) for artifacts and scope.
Run the Ceedling SDIO read-batch regressions for DMA boundary and recovery
checks; see [firmware test guidance](../../../firmware/tests/README.md#host-behavior-regressions).
At 200 MHz with SDIO fixed at 12.5 MHz, a separate run reached 2.55 MiB/s read,
indicating some CPU-side cost, but the overclock is diagnostic only.

A small HID-started run also passed. A separate cancellation check confirmed
duplicate starts and MSC enable were rejected during writes, and Cancel removed
the partial test file and unmounted successfully. The .NET dashboard started and
completed its own 1 MiB hardware run. Desktop/mobile screenshots and runtime
checks showed no horizontal overflow; all dashboards require explicit write
consent. Direct WebHID protocol/client/rendering tests and disconnected browser
layout passed, but native browser-to-device WebHID access was not exercised by
the automated browser in this environment. The integrated browser/CDP connection
was unavailable, so local Playwright/Chromium was used.

At the time of that validation, the .NET development launch encountered a static-web-assets
path issue in this workspace; its normal launch and generated OpenAPI endpoint
test were verified. Those runs did not qualify SPI benchmark performance, physical direct-WebHID access,
long-duration endurance, media removal, or power-loss recovery. Later SPI measurements are recorded in the
[transport qualification notes](sdcard-bringup-observations.md#spi-clock-qualification-2026-10-17).

For the MSC reset-sense review and its validation limits, see the
[2026-10-15 static review record](2026-10-15-static-sd-review-followup.md).
