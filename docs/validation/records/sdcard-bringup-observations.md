# SD-Card Bring-Up Observations

Historical observations extracted from the physical-link reference. Each section retains its original configuration
and limitations; these results do not qualify current firmware.

## SPI Clock Qualification (2026-10-17)

The connected SanDisk `SB128` (128 GB, product revision 8.0) returned a CRC-valid CMD6 check status with function-group-1
support mask `0x8001`: Default Speed is supported, but the High-Speed function-1 bit is clear. The driver therefore
retained 25 MHz even with the board's 50 MHz request. This establishes this card's advertised SPI limit, not the limits
of its native SD mode or every card with the same product name. The initial switch from native firmware failed at
CMD58; card removal/reinsertion followed by a firmware reset restored SPI initialization. The final SPI argument
also clears ACMD41's reserved voltage-window bits; that change alone was not isolated as the cause of recovery.

Clean version `0.1.0` firmware passed the filesystem write/read self-test on the existing exFAT volume:

| Pico 2 SYSCLK / actual SPI SCK | Test size | Write / read | Result |
| --- | --- | --- | --- |
| 150 MHz / 25 MHz | 128 MiB | 1.91 / 1.98 MB/s | Passed; zero mismatches and errors; test file removed |
| 200 MHz / 25 MHz | 32 MiB | 2.02 / 2.10 MB/s | Passed; zero mismatches and errors; test file removed |

Only `/PFTEST.BIN` was used, without formatting or replacing other files. The board was left at stock 150 MHz with
25 MHz SPI, the filesystem mounted, and MSC disabled. CMD6 check/switch sequencing, CRC/error rejection, rate rounding,
and unsupported-card limits have host regression tests. A card advertising SPI High Speed is still required to
qualify physical 37.5/41.67/50 MHz operation; no 50 MHz SPI hardware pass is claimed here.

## Historical SD-Card Qualification

These records describe the firmware, cards, clocks, and wiring tested at the time. They are not qualification of the
current transport extraction or a throughput guarantee. See the
[SD-card validation limits](../../reference/sdcard/driver.md#validation-and-limits) for current test scope.

Pico 2 SDIO initialization was verified with the connected 16 GB card: UART reported `SD card initialized`, and HID
reported `initialized=true`, 30,875,648 sectors (15,808,331,776 bytes), and a 12.5 MHz bus clock. The command PIO drove
all 48 command bits before rising clock edges and captured the response start bit without a software bit shift.
Command completion allowed at least eight idle clock cycles before the next command. That initial check did not
exercise sector I/O or filesystem mounting and wrote no card data; the full SDIO CLI qualification is recorded below.

On 2026-10-13, the same Pico 2 and card were verified in SPI mode at 25 MHz after reboot. HID reported the same capacity
and CID. The SPI driver explicitly pulled MISO high so released responses had a defined idle level; this test does not
isolate that change from the reboot as the cause of successful initialization. The initial read-only CDC CLI check
successfully mounted the existing filesystem, listed `/`, read directory metadata with `fs stat`, and reported
total/free filesystem space with `fs df` (15,799,943,168 / 15,799,877,632 bytes). These operations exercised SPI sector
reads and CRC checks. HID identity and runtime/memory/MSC input reports remained accessible during CDC use. The
filesystem was explicitly unmounted afterward, with MSC disabled and Pico retaining card ownership. No files were
changed or formatted during that initial check. SPI was explicitly selected for this qualification; select the intended
transport before building.

A subsequent authorized CDC CLI qualification on the same date passed 36 checks using only disposable files in
`/pf-cli-test-e700f283`. Verified commands were `help fs`, `sd status`, `fs mount`, `unmount`, `ls`, `stat`,
bounded/default `cat`, `df`, `mkdir`, `write`, `append`, `mv`, and confirmed `rm`. Writing and appending produced the
exact expected readback (`hello world more text`, 21 bytes). Guard checks refused operations before mount, duplicate
creation, write/rename overwrites, unsafe paths, invalid read limits, unconfirmed deletion, and directory deletion.
HID identity and runtime/memory/MSC reports continued throughout. Both test files were deleted and their absence
verified; only the empty test directory remained. The filesystem was unmounted, MSC disabled, and Pico retained
ownership. Existing files were not changed and media was not formatted. This verifies the tested small-file SPI
workflow, not sustained-write endurance, power-loss recovery, detailed backpressure, or the SPI MSC handoff lifecycle.

The same 36-check suite subsequently passed in native SDIO mode at 12.5 MHz on 2026-10-13. Switching from SPI first
failed CMD8 after a warm MCU reset; a full card power cycle restored native initialization. Initial directory creation
then failed with `DISK_ERR`. The write PIO had treated the CRC-status end bit as ready before the card entered its busy
phase, allowing a subsequent CMD24 too early. The corrected program waited through the following falling/rising clock
edges before waiting for DAT0 to release busy. Write start/stop and bus release also transitioned on falling edges.
The focused directory write then succeeded, and the full suite passed on clean firmware with temporary diagnostics
removed. Readback again matched `hello world more text` (21 bytes), all safety guards passed, and HID
identity/runtime/memory/MSC reports stayed available. Test files were deleted and absence verified; empty
`/pf-sdio-test-2efe1ac8` and diagnostic `/pf-sdio-diag-12d12bef` directories remained alongside the earlier SPI test
directory. No existing file contents were changed and no formatting was done. SDIO remained selected, the filesystem
was unmounted, MSC was disabled, and Pico owned the card. These checks do not qualify endurance, power-loss recovery,
high-speed timing margins, or complete SDIO MSC handoff workflows.

## Class 4 Read-Only Bring-Up (2026-10-15)

The inserted Class 4 card initially reported `present=yes`, `initialized=0`, `mounted=0`, with native initialization
failing at SCR stage 17. Finer diagnostics isolated a repeatable SCR CRC mismatch: payload `015a800000000000`, received
CRC `023a`, computed CRC `8a2a`. These bits are the start bit followed by the valid SCR `02b5000000000000` and its CRC
`0475`, truncated by one bit. The receiver was detecting DAT0 asynchronously and counting the start bit as payload.
Correcting start detection then exposed a malformed CMD12 response during the third 64-sector qualification batch;
command-response sampling also needed moving away from the rising edge.

With falling-edge-qualified receive start detection and falling-edge command response sampling, the connected Pico 2
at stock 150 MHz initialized the card at 25 MHz, mounted the existing filesystem, and listed `/` successfully. Capacity
was 30,875,648 sectors / 15,808,331,776 bytes. Clean firmware, without temporary UART instrumentation, passed ten
consecutive `fs unmount` / `fs mount` / `sd status` / `fs ls /` cycles. Each mount reinitialized the card and exercised
its 256-sector CRC-checked read qualification. Final diagnostics were `sequence=0 operation=none result=ok`, with the
card left initialized and mounted, MSC disabled, and Pico ownership.

No formatting, repair, file creation, or write self-test was performed. This qualifies the observed read-only bring-up
on this card and wiring, not sustained writes, another card, high-speed timing margins, or power-loss recovery. The
Class 4 marking is a speed rating, not the cause of the initialization failure. This historical record does not change
the current SDHC/SDXC Class 10-or-better target or its initialization policy.

## TX Completion Regression (2026-10-17)

The replacement 8 GB `SD8GB` card initialized and mounted exFAT at 25 MHz, but writes failed with TXSTALL. Both TX
programs signaled completion and wrapped into an empty blocking PULL before the CPU disabled the state machine.
Parking on `IRQ WAIT 2` removes that false underrun without weakening actual starvation detection or changing wire timing.

The fixed image was flashed and verified on Pico 2 at 150 MHz CPU / 25 MHz SD. An authorized 1 MiB file write/read
self-test passed with zero mismatches and cleanup (2.55 MB/s write, 7.44 MB/s read). The dashboard subsequently
reported a passed 32 MiB test with zero mismatches and cleanup (7.19 MB/s write, 7.48 MB/s read). Final card diagnostics
had no CRC, timeout, or FIFO-stall errors, and `/PFTEST.BIN` was absent. These checks do not qualify other cards,
electrical timing margins, optional 50 MHz operation, endurance, or power-loss safety.

## High-Speed Bring-up (2026-10-17)

The same card exposed a one-bit-shifted SCR at 200 MHz CPU / 400 kHz SD before the Default-Speed RX edge correction
above. A fixed payload-only settling delay was insufficient. High-Speed qualification also failed without additional
settling after CMD6 selected the new function. The SD policy now leaves CLK running for 1 ms at the old rate after
DAT0 becomes ready, before raising the clock. This is a bounded lab-qualified delay, not a guarantee for every card.

At 200 MHz CPU / 50 MHz SD, three authorized 32 MiB write/read tests passed, including two intervening normal resets.
Each wrote and read all 33,554,432 bytes with zero mismatches and removed `/PFTEST.BIN`. Write throughput was
7.92-8.07 MB/s and read throughput 10.92 MB/s. GPIO drive/slew defaults were retained; no debugger qualification
breakpoints were used for these three tests. The user chose to leave this image running.

Before final inspection the card was changed to the 128 GB `SB128`. The dashboard separately reported a passed
128 MiB self-test with zero mismatches and cleanup at the same 200 MHz CPU / 50 MHz SD setting (15.47 MB/s write,
14.00 MB/s read). Final CDC inspection confirmed High-Speed timing, no CRC/timeout/stall errors, no fallback,
facade diagnostics `result=ok`, and `/PFTEST.BIN` absent. This was a separate dashboard test, not one of the three
controlled runs above.

The 250 MHz CPU / 50 MHz SD configuration still failed runtime I/O despite successful negotiation and occasional
valid sector captures. A captured CMD17 frame was correct at the MCU pins, but the card reported `COM_CRC_ERROR`;
this does not establish signal timing at the card socket. Neither fractional timing nor wiring is individually
proven to be the remaining cause. Do not treat a mounted 50 MHz volume, or a passing timing model, as sustained
qualification. CRC checks, the one-time startup fallback, and runtime invalidation remain enabled.

The tested image used the now-removed build-time clock override. Reproduce that exact historical image only from
its recorded revision. Current builds use the SDK default clock; choose the saved 200 MHz system-clock and
High-Speed storage profiles through [boot settings](../../reference/settings.md) to exercise those targets on current firmware.
This does not reproduce the historical binary or qualify the current firmware.

