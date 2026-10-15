# SD / MSC review fixes and host validation

Starting point: `performance-enhancement`, `c293ed3acdd4d593e731070821a512bd15e09a86`.
No device was accessed, no firmware flashed, no media operation performed.

## SPI write outcomes

CMD24 and CMD25 use the same production `write_data_block`. Accepted response plus
ready is success. CRC error (0x0b) or write error (0x0d) plus ready is a confirmed
rejection: return I/O error but retain initialization. Missing/malformed response,
short token/data transfer, or ready timeout invalidates initialization. Subsequent
reads/writes/sync return NOT_READY until explicit initialization. No write replay.
CMD25 still runs its stop cleanup on failure, returns failure even when cleanup
succeeds, and invalidates on failed stop. Cleanup cannot erase an uncertain write.
A sync ready timeout also invalidates initialization.

## MSC contract and ownership

TinyUSB invokes `tud_msc_scsi_cb` for unsupported built-ins: negative return fails,
zero succeeds for no-data commands. SYNCHRONIZE CACHE(10), opcode 0x35, synchronously
calls the owner-checked block sync. Whole-device sync covers any requested range.
IMMED/reserved bits and unexpected data length fail with INVALID FIELD IN CDB;
unavailable medium fails NOT READY; backend sync failure fails MEDIUM ERROR.
Other unsupported commands retain INVALID COMMAND OPERATION CODE.

PREVENT now rejects eject with 0x53/0x02 before sync or changing ejected state.
Sync failure also leaves ejected false. Allow followed by successful eject marks
the medium ejected; the existing task request separately transfers ownership.
It refuses busy workers and rolls back enabled state on owner handoff failure.
FatFs is already unmounted before USB ownership is acquired; eject must not call
FatFs or mount a filesystem owned by the host.

MSC callbacks, HID enable requests and `pf_usb_msc_task` all run in the USB task
(`usb/task.c` calls tud_task then msc_task). They cannot concurrently mutate MSC
flags. Storage worker and removal/telemetry/handoff share the recursive card
reservation through the facade. No new concurrent flag writer is introduced.

## Minimal performance candidate and limits

Removed the redundant sync in WRITE10-complete. TinyUSB invokes this void callback
only after CSW has been sent; it cannot report a sync failure. MSC writes one block
per callback, and both production transports check the data response and busy
completion before returning write success. There is no application write cache.
Explicit SCSI sync, eject and ownership-release sync remain checked boundaries.
SDIO multi-block CMD12 recovery and all CRC, PIO/DMA validation remain unchanged.

For SPI the removed call performed an extra select, ready poll and deselect/trailing
clock per completed WRITE10 command. For SDIO it acquired the reservation and
called recover even though MSC's single-block writes had completed. Host regression
asserts zero sync calls for successful WRITE10 plus complete, compared to one in
the old callback. This is a reproducible reduction in calls, not measured MB/s.
It does not optimize the firmware FatFs self-test path. SDIO already has batching,
slicing-by-4 CRC/lane mapping and optional command/data/CRC/busy/stop/recovery
profiling; timings overlap and cannot be summed. No speculative further driver
micro-optimization was applied without hardware phase evidence.

The cited 2026-10-15 three-run 7.712/8.311 MB/s record was not present at this HEAD.
The available 2026-10-14 record is historical firmware data, not a new candidate
result or hardware A/B. Copilot must build/flash and compare the same card/config
(Pico2 150 MHz, SDIO 25 MHz), with identical file-level timing/verification and
three 128 MiB runs. Create only unique temporary test files, verify all bytes,
sync/close, remove only those files, and preserve all existing card files. To
measure this MSC candidate specifically, also compare the same host file-level
MSC workload; firmware self-test alone cannot establish its benefit.

## Reproduction

- `python3 tools/validation/check-sd-msc-production.py`: compiles complete, unedited
  production SPI and MSC sources against hardware/storage seams, links real SPI
  protocol and CRC implementation, and executes assertions. No source extraction
  or parser defines the behaviors. Both suites were observed failing before fixes.
- `python3 tools/validation/check-storage-automount.py`: connected lifecycle checks,
  updated to require PREVENT eject refusal.
- Run existing serialization, SDIO init/TX/read-batch, SPI batch and PIO timing
  validations and installed Ceedling `test:all` (no installer needed).

Host assertions cover accepted/rejected/missing/malformed writes, busy timeouts,
short transfers, CMD25 cleanup, subsequent sync/I/O refusal, SCSI success/failure,
unsupported/invalid commands, absent medium, prevention/allow, worker busy and
owner handoff failure. Existing serialization tests exercise simultaneous facade
I/O versus removal/telemetry/handoff. PIO timing is a model, not hardware proof.

Final host results: new production suites PASS; connected lifecycle PASS;
serialization, SDIO initialization, TX, read-batch, SPI batch and assembled PIO
model checks PASS. Installed Ceedling 1.1.10 `test:all`: 120 tests, 0 failures.
Shell syntax and `git diff --check` PASS. The first Ceedling attempt exposed a
stub stdlib header search collision; isolated host tests were moved under
`tools/validation/host`, and the complete suite was rerun successfully.
Firmware build and all physical validation are deferred to Copilot as requested.
