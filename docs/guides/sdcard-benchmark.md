# SD-Card Performance Test

This guide owns the benchmark's safety rules, execution workflow, measurement boundaries, and result interpretation.
For card-driver behavior, see the [SD-card driver design](../reference/sdcard/driver.md);
for wiring and transport constraints, see [SD-card physical links](../reference/sdcard/physical-links.md).

The application implementation lives in `app/storage/file_benchmark.c/.h` and uses `pf_file_benchmark_*` APIs.
It operates on the already-mounted SD-card volume; the existing host UI/API calls it a
self-test. The [storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle) mounts the filesystem at
startup and after normal USB return independently of Start; healthy firmware-owned storage stays mounted.
HID Input report `11` carries state and progress; Feature report `20`, opcode `4`, starts or cancels it.
The numeric state/error values retain their documented assignments: wire state 2 (`preparing`) means the worker is
preparing an accepted run against the existing mount, and wire error 5 (`card`) means the selected media is unavailable.

Current source-review dispositions and remaining qualification: [SD remediation matrix](../validation/sd-review.md).

The firmware can write a dedicated file, read it back, verify every byte, and
report average filesystem write/read speeds over HID. The Python and .NET
dashboards and the Direct dashboard expose Start, Cancel, progress, timings,
byte counts, average MiB/s, verification results, and cleanup errors.

## Running

The benchmark is optional: CMake defaults `PF_ENABLE_STORAGE_BENCHMARK` to `OFF`. The development
`tools/firmware/build.sh` script explicitly enables it; `tools/firmware/load.sh` delegates builds to that script
(loading with `--skip-build` does not change the existing image).
For a benchmark-free image, configure directly with CMake using `-DPF_ENABLE_STORAGE_BENCHMARK=OFF` and build
with `cmake --build <build-dir>`; do not rebuild that configuration through the development script.
Disabled builds omit the benchmark implementation and its 32 KiB buffer, clear the HID self-test capability,
reject start/cancel commands as unsupported, and do not publish self-test reports. Host interfaces use that
capability to disable self-test controls. Normal storage, filesystem commands, and MSC remain available.

1. Keep the card owned by Pico: safely eject and disable MSC if it is enabled.
2. Wait for automatic mounting and fresh mounted-filesystem status. If mounting fails or was deliberately suppressed,
   inspect the storage error first; `storage mount` is a manual retry, not a routine prerequisite for each test.
3. Open one dashboard and connect to its vendor HID interface.
4. Choose an integer file size from 1 to 1024 MiB; the default is 32 MiB.
   The test streams through its fixed buffer, not a file-sized RAM allocation. Larger tests take longer and
   write more data; sufficient free space and (when enabled) contiguous preallocation are still required.
   Update firmware and host software together to use sizes above the previous 128 MiB limit.
5. Select **Start benchmark** once. The firmware checks the reserved path and free space before accepting the write;
   no existing file is replaced and no mount-only step occurs. Free-space scanning can delay acceptance and byte
   progress, particularly on large filesystems; do not submit a second Start while its command result is pending.
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
  that byte counter. The initial free-space check happens before the run is accepted, so it is not necessarily
  represented by the self-test's Preparing state. The
  [storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle) keeps the command pending during preflight;
  this is not a distinct wire phase.

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
per-phase timing, see
[performance controls and status](../reference/sdcard/driver.md#performance-controls-and-status).

## CMD25 Persistence Decision

Native SDIO CMD25 persistence and checked stream recovery are defined by the
[driver write workflow](../reference/sdcard/driver.md#write-workflow).
The [controlled A/B record](../validation/records/2026-10-15-cmd25-persistence.md) preserves the measured decision and
its configuration-specific limits; it is not a universal throughput guarantee.

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
See the [HID Report Reference](../reference/usb-hid.md) for the wire contract.

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

## Measurement Records

Configuration-specific [benchmark observations](../validation/records/benchmark-observations.md) and
[test records](../validation/records/README.md) preserve earlier results; they do not qualify current firmware.
