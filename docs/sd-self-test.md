# SD Card Self-Test

The firmware can write a dedicated file, read it back, verify every byte, and
report average filesystem write/read speeds over HID. The Python and .NET
dashboards and the direct WebHID page expose Start, Cancel, progress, timings,
byte counts, average MiB/s, verification results, and cleanup errors.

## Running

1. Keep the card owned by Pico: safely eject and disable MSC if it is enabled.
2. Unmount any CLI/application filesystem using `fs unmount`.
3. Open one dashboard and connect to its vendor HID interface.
4. Choose an integer file size from 1 to 128 MiB; the default is 32 MiB.
5. Check **Allow temporary SD-card writes**, then select **Start self-test**.
6. Wait for **Passed**, **Failed**, or **Cancelled**; Cancel requests cleanup and
   does not make ownership available until the worker finishes.

Do not reset, unplug, remove the card, or change transport while the test runs.
This test writes flash and contributes to card wear. Back up important data
before testing. It is not a filesystem repair or full-capacity authenticity test.

The fixed filename is `/PFTEST.BIN`. The firmware uses create-new mode and refuses
to run if that name already exists, regardless of capitalization on FAT. It
never overwrites or deletes a pre-existing file. Insufficient free space is
rejected before creation. The test's own file is closed, removed, and the volume
unmounted before completion is published. If close/unlink/unmount fails, explicit
status flags identify a possible remaining test file or mounted volume. An
interrupted/power-lost run may leave the file; inspect it before deliberately
removing it with the CLI. Do not assume cleanup after a reset.

Only one storage operation can be outstanding. CLI storage commands, duplicate
test starts, and MSC transitions are refused during the benchmark. Starting on
an already mounted volume is refused rather than automatically unmounting another
client's filesystem. Closing the dashboard or CDC does not cancel a HID-started
test: use Cancel. Results persist until the next accepted test or a device reset.

Use one active host monitor per board. On Linux, Python's libusb-backed HIDAPI can
detach the kernel HID driver needed by HidSharp/.NET. Stop the conflicting monitor
and reconnect the safely idle board before changing host implementation.

## Measurement

The worker uses a statically allocated, aligned 8 KiB buffer. Every four-byte word
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
- Mount, free-space scanning, file open, deletion, and unmount are not included
  in phase averages.
- Progress is `(written + read) / (2 * target)`; writing occupies the first half
  and verification the second half. Mount/cleanup may take time without advancing
  that byte counter.

These are application-level filesystem throughput measurements, not raw wire
bandwidth. They include the firmware's current single-sector backend behavior
and card programming delays. Results depend on card, transport, bus speed,
allocation state, and runtime load. This implementation does not perform
multi-block throughput optimization, endurance qualification, or power-loss tests.

The storage worker performs all FatFs operations; the USB task owns HID/CDC.
Progress is copied under a short FreeRTOS critical section and emitted through
HID Input Report 6 every 500 ms. Cancellation is checked between chunks and
before creation; a current I/O operation completes before cleanup. Existing
8 KiB worker stack allocation remains unchanged, with an additional 8 KiB static
data buffer. No benchmark data is sent through HID or CDC.

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
The direct browser sends the same commands using HID Feature Report 6.
The wire layout is authoritative in [USB protocol](usb-protocol.md#report-id-6).

## Validation

Host-independent engine tests cover deterministic writes/readback, corruption,
short writes, cancellation, existing-file protection, insufficient space, sync
and close failures, and failed cleanup:

```sh
tools/validation/setup-ceedling-testing.sh test:test_self_test
```

Host tests cover report layout/validation, average calculations, start/cancel
encoding, API status codes and OpenAPI generation, consent gating, progress,
cleanup messages, and direct-client event handling. Run them through
`tools/validation/test-host.sh` using the configured Python environment.

On 2026-10-13 the connected Pico 2 and 16 GB card passed a real 32 MiB run in SDIO
mode at 12.5 MHz, started through the Python dashboard in Chromium:

| Metric | Result |
| --- | --- |
| Written and verified | 33,554,432 bytes each |
| Write time | 391,152 ms |
| Read/verify time | 60,402 ms |
| Average write | 0.082 MiB/s |
| Average read | 0.530 MiB/s |
| Mismatched bytes | 0 |
| Cleanup | File removed, volume unmounted |

A small HID-started run also passed. A separate cancellation check confirmed
duplicate starts and MSC enable were rejected during writes, and Cancel removed
the partial test file and unmounted successfully. The .NET dashboard started and
completed its own 1 MiB hardware run. Desktop/mobile screenshots and runtime
checks showed no horizontal overflow; all dashboards require explicit write
consent. Direct WebHID protocol/client/rendering tests and disconnected browser
layout passed, but native browser-to-device WebHID access was not exercised by
the automated browser in this environment. The integrated browser/CDP connection
was unavailable, so local Playwright/Chromium was used.

The .NET development launch currently encounters an existing static-web-assets
path issue in this workspace; its normal launch and generated OpenAPI endpoint
test were verified. SPI benchmark performance, physical direct-WebHID access,
long-duration endurance, media-removal, and power-loss recovery remain unverified.