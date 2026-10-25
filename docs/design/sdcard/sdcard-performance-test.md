# SD-Card Performance Test

This guide owns the benchmark's safety rules, execution workflow, measurement boundaries, and result interpretation.
For card-driver behavior and transport constraints, see the [SD-card driver design](sdcard-driver-design.md); for the
full SD documentation map, see the [SD-card documentation index](README.md).

The application implementation lives in `app/storage/file_benchmark.c/.h` and uses `pf_file_benchmark_*` APIs.
It operates on the already-mounted SD-card volume; the existing host UI/API calls it a
self-test. HID Input report `11` carries state and progress; Feature report `20`, opcode `4`, starts or cancels it.
The numeric state/error values retain their documented assignments: wire state 2 (`preparing`) means the worker is
preparing an accepted run against the existing mount, and wire error 5 (`card`) means the selected media is unavailable.

Current source-review dispositions and remaining qualification: [SD remediation matrix](../../tests/sd-design-remediation.md).

The firmware can write a dedicated file, read it back, verify every byte, and
report average filesystem write/read speeds over HID. The Python and .NET
dashboards and the direct WebHID page expose Start, Cancel, progress, timings,
byte counts, average MiB/s, verification results, and cleanup errors.

## Running

1. Keep the card owned by Pico: safely eject and disable MSC if it is enabled.
2. Ensure the filesystem is mounted; use `fs mount` if it is not already mounted.
3. Open one dashboard and connect to its vendor HID interface.
4. Choose an integer file size from 1 to 1024 MiB; the default is 32 MiB.
   The test streams through its fixed buffer, not a file-sized RAM allocation. Larger tests take longer and
   write more data; sufficient free space and (when enabled) contiguous preallocation are still required.
   Update firmware and host software together to use sizes above the previous 128 MiB limit.
5. Select **Start self-test**. The firmware checks the reserved path and free space before accepting the write;
   no existing file is replaced.
6. Wait for **Passed**, **Failed**, or **Cancelled**; Cancel requests cleanup and
   does not make ownership available until the worker finishes.

Do not reset, unplug, remove the card, or change transport while the test runs.
This test writes to the SD card and contributes to card wear. Back up important data
before testing. It is not a filesystem repair or full-capacity authenticity test.

The fixed filename `/PFTEST.BIN` is reserved for self-tests. **Starting a test
rejects any existing file or directory at that path**, regardless of
capitalization on FAT. Remove a leftover from an interrupted run manually after
checking its contents; the firmware never replaces it. A fresh file is created
in create-new mode and removed during final cleanup; the filesystem remains
mounted. If close/unlink fails, explicit status flags identify a possible
remaining test file. Do not assume cleanup after a reset.

Only one storage operation can be outstanding. CLI storage commands, duplicate
test starts, and MSC transitions are refused during the benchmark. Starting on
an unmounted volume is refused, and Start is disabled until mounted-filesystem
telemetry arrives. The test never mounts or unmounts the volume. Closing the dashboard or CDC does not cancel a HID-started
test: use Cancel. Results persist until the next accepted test or a device reset.

Use one active host monitor per board. On Linux, Python's libusb-backed HIDAPI can
detach the kernel HID driver needed by HidSharp/.NET. Stop the conflicting monitor
and reconnect the safely idle board before changing host implementation.

## Measurement

The worker uses a statically allocated, aligned 32 KiB buffer. Every four-byte word
contains its file offset XORed with a fixed pattern, so block-offset mistakes and
corruption can be detected. The entire requested file is verified, not sampled;
a mismatch fails the run. Read/write short transfers, sync errors, and close
errors also fail the run.

Average speed is transferred bytes multiplied by 1000, divided by elapsed
milliseconds. Dashboards display MiB/s using 1 MiB = 1,048,576 bytes; zero-duration
phases display no speed rather than an infinite or invented value.

- Write timing includes pattern generation, filesystem writes, final `f_sync`,
  and `f_close`.
- Read timing includes filesystem reads, verification, and `f_close`.
- Free-space scanning, file open, and deletion are not included
  in phase averages.
- Progress is `(written + read) / (2 * target)`; writing occupies the first half
  and verification the second half. Preparation/cleanup may take time without advancing
  that byte counter.

These are application-level filesystem throughput measurements, not raw wire
bandwidth. SDIO contiguous writes use CMD25 and reads use CMD18 batches of up to
64 sectors; single-sector tails use CMD17/CMD24. SPI multi-sector requests use
CMD18/CMD25 transactions and single sectors use CMD17/CMD24. Results depend on card, transport, bus speed,
allocation state, and runtime load. The implementation does not qualify
endurance or power-loss behavior.

## Contiguous Benchmark Mode

The default test uses normal FatFs allocation. Configure
`-DPF_STORAGE_SELF_TEST_PREALLOCATE=ON` to call `f_expand()` after creating
the new test file and before starting write timing. The call asks FatFs for a
contiguous allocation. Failure is reported as a filesystem error and still
cleans up the newly created test file. Allocation time is
excluded from write averages, so record preallocated and normal results
separately. This mode changes allocation and wear behavior, may need contiguous
free space, and is not a raw-driver benchmark. The driver has no phase profiling; use an external trace for
per-phase timing, see [performance controls and status](sdcard-driver-design.md#performance-controls-and-status).

## CMD25 Persistence Decision

Native SDIO keeps a successful CMD25 stream open across consecutive facade
writes. This behavior is unconditional: there is no build option to close the
stream after every facade call. A non-contiguous operation, synchronization,
error, removal, or reinitialization still performs the required checked stream
recovery.

The [2026-10-15 controlled A/B measurement](../../tests/records/2026-10-15-cmd25-persistence.md)
found a 40.93% median write-throughput gain from persistence across six passing
128 MiB runs at 25 MHz SDIO with contiguous preallocation. This was well above
the 5-10% threshold for retaining the additional stream state. See that record
for the historical test variants, raw data, controls, and qualification limits.

The storage worker performs all FatFs operations; the USB task owns HID/CDC.
Progress is copied under a short FreeRTOS critical section and emitted through
HID Input report `11` every 500 ms when the board advertises self-test capability. Start is preflighted by the
storage worker while it owns admission; the prior result remains visible until the file/path and free-space checks
succeed. Cancellation is checked between chunks and before creation; a current I/O operation completes before
cleanup. The 8 KiB
worker stack allocation remains unchanged. The test uses a 32 KiB static buffer;
SDIO batched reads use an additional 33,280-byte (32.5 KiB) static wire buffer.
The SDIO CRC lookup tables occupy another 8 KiB. No benchmark
data is sent through HID or CDC.

## API

Both local hosts accept `POST /api/self-test` and return HTTP 202 to acknowledge
command delivery, not successful completion:

```json
{"action":"start","size_mib":32}
```

```json
{"action":"cancel"}
```

Poll `/api/status` and its `self_test` object. Firmware rejection is reported by
`request_error`; execution failure uses `error` and `filesystem_result`.
The HID command result reports Pending while start preflight runs, then Completed when the test is accepted or Failed
for preflight I/O, card, space, or existing-path errors. Input report `11` retains the previous test result for
rejected starts and carries progress after acceptance. Storage status is reported independently in Input report `10`.
See the [HID Report Reference](../usb/usb-hid-report-reference.md) for the wire contract.

## Validation

Host-independent engine tests cover deterministic writes/readback, corruption,
short writes, cancellation, existing-file rejection, insufficient space, sync
and close failures, and failed cleanup:

```sh
tools/validation/setup-ceedling-testing.sh test:test_file_benchmark
```

Host tests cover report layout/validation, average calculations, start/cancel
encoding, API status codes and OpenAPI generation, mounted-volume admission, progress,
cleanup messages, and direct-client event handling. Run them through
`tools/validation/test-host.sh` using the configured Python environment.

### Historical Measurements

The observations below describe earlier firmware and test workflows, including older buffer sizes and unmount
behavior. They do not override the current [running safeguards](#running) or [measurement boundaries](#measurement).
For later card/clock comparisons, see the
[2026-10-18 speed matrix](../../tests/records/2026-10-18-sdio-speed-matrix.md) and its
[repeat measurement](../../tests/records/2026-10-18-sdio-speed-matrix-repeat.md).

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
[the read-throughput record](../../tests/records/2026-10-13-sdio-read-throughput.md) for artifacts and scope.
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
[transport qualification notes](../../../firmware/src/drivers/sdcard/README.md#spi-clock-qualification-2026-10-17).

For the MSC reset-sense review and its validation limits, see the
[2026-10-15 static review record](../../tests/records/2026-10-15-static-sd-review-followup.md).
