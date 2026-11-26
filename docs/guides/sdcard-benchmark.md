# SD-Card Performance Test

[Documentation](../README.md) / [User guides](README.md)

This guide owns the benchmark's safety rules, execution workflow, measurement boundaries, and result interpretation.
For card-driver behavior, see the [SD-card driver design](../reference/sdcard/driver.md);
for wiring and transport constraints, see [SD-card physical links](../reference/sdcard/physical-links.md).

The host UI/API calls this benchmark a self-test. It uses the already-mounted volume; Start does not mount it.
See the [storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle) for mounting policy and the
[HID reference](../reference/usb-hid.md#report-id-11-storage-benchmark-status) for report layouts and numeric errors.

Current source-review dispositions and remaining qualification: [SD remediation matrix](../evidence/sd-review.md).

The firmware can write a dedicated file, read it back, verify every byte, and
report average filesystem write/read speeds over HID. The Python and .NET
dashboards and the Direct dashboard expose Start, Cancel, progress, timings,
byte counts, average MiB/s, verification results, and cleanup errors.

## Running

The benchmark is an optional module: the build wrapper includes it unless `--benchmark OFF` is given; direct CMake
builds omit it by default. See [build configuration](../reference/build-configuration.md#optional-modules).
Without benchmark support, dashboards disable its controls; normal storage, filesystem commands and MSC remain available.

1. Keep the card owned by Pico: safely eject and disable MSC if it is enabled.
2. Wait for automatic mounting and fresh mounted-filesystem status. If mounting fails or was deliberately suppressed,
   inspect the storage error first; `storage mount` is a manual retry, not a routine prerequisite for each test.
3. Open one dashboard and connect to its vendor HID interface.
4. Choose an integer file size from 1 to 1024 MiB; the default is 32 MiB.
   The test streams through its fixed buffer, not a file-sized RAM allocation. Larger tests take longer and
   write more data; sufficient free space and (when enabled) contiguous preallocation are still required.
5. Select **Start benchmark** once. The firmware checks the reserved path and free space before accepting the write;
   no existing file is replaced and no mount-only step occurs. Free-space scanning can delay acceptance and byte
   progress, particularly on large filesystems; do not submit a second Start while its command result is pending.
6. Wait for **Passed**, **Failed**, or **Cancelled**; Cancel requests cleanup and
   does not make ownership available until the current I/O and worker cleanup finish.

Rejected starts leave the previous result visible; check the command error rather than interpreting an old Passed
result as a new completed run.

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
bandwidth. Only progress and results go to the host; benchmark file data stays on the board/card path.
SDIO contiguous writes use CMD25 and reads use CMD18 batches of up to
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

## Implementation and Checks

- [Self-test HTTP API](../development/host-tools.md#self-test-api) and
  [HID reports](../reference/usb-hid.md#report-id-11-storage-benchmark-status)
- [Driver write workflow](../reference/sdcard/driver.md#write-workflow) and
  [resource budget](../reference/sdcard/driver.md#peripheral-and-memory-budget)
- [Benchmark build options and focused checks](../reference/build-configuration.md#validate-the-selected-variant)

## Measurement Records

Configuration-specific [benchmark observations](../evidence/records/benchmark-observations.md) and
the [CMD25 A/B record](../evidence/records/2026-10-15-cmd25-persistence.md) preserve earlier results; they do not
qualify current firmware. See [all records](../evidence/records/README.md) for other configurations.
