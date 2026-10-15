# SD facade simplification and validation record — 2026-10-15

Base: `performance-enhancement`, HEAD `aaab8b8769bb69dbe05cc7c7da01335bbb4b0d71`.

The facade simplification, CMD24 DATA/TIMEOUT profiling, expanded tests and original validation record were preserved. This follow-up addresses the review items below with bounded changes to MSC callbacks, SPI error propagation, profile documentation and host regressions; it does not claim hardware qualification or measured performance gain.

## Confirmed validation fixes

- `check-sd-msc-production.py` now fails before running checks if either required production regression source is missing. It prints a path-specific `FileNotFoundError`; it no longer silently skips an input.
- `check-msc-elf.py` is an independent ELF-only entry point. It does not build firmware or invoke host regression tests. It checks each requested ELF, prints its canonical path and SHA-256, rejects duplicate file identities including symlink/hardlink aliases, and reports failures with a nonzero exit. A hash identifies bytes, not transport provenance.
- `check-validation-integrity.py` uses temporary repository-source copies to exercise the actual production-runner missing-input checks without unlinking or moving worktree files. It also tests two synthetic ARM ELF/DWARF artifacts with the installed ARM binutils, two distinct artifacts in one invocation, symlink/hardlink duplicates, corrupted wrapper pointers, malformed layouts, invalid/missing artifacts, and hash output.
- SPI deselect trailing-clock short-return behavior remains deferred. The checked-in Pico SDK blocking SPI source returns the requested byte count after its transfer loop; short returns were injectable only through the host substitute. This does not establish physical trailing-clock failure behavior, so no firmware change was made.

## Verification performed

From the repository root:

- `python3 -m py_compile tools/validation/check-sd-msc-production.py tools/validation/msc_elf.py tools/validation/check-msc-elf.py tools/validation/check-validation-integrity.py` — passed.
- `python3 tools/validation/check-validation-integrity.py` — 2 tests passed.
- `python3 tools/validation/check-sd-msc-production.py` — required SPI-write and MSC production regressions both executed and passed; pinned TinyUSB source-order checks passed (callback model, not BOT integration).
- `python3 tools/validation/check-sd-serialization.py` — passed for SDIO and SPI facade configurations.
- `python3 tools/validation/check-sdio-tx.py` — passed CMD24 DATA/TIMEOUT accounting and CMD25 checks.
- `python3 tools/validation/check-sd-spi-transfer.py` — passed.
- `python3 tools/validation/check-sd-spi-read-errors.py` — passed.
- `python3 tools/validation/check-sdio-init.py` — passed.
- `python3 tools/validation/check-sdio-pio-timing.py` — passed as a model; not hardware timing evidence.
- `python3 tools/validation/check-sdio-read-batch.py` — passed.
- `python3 tools/validation/check-storage-automount.py` — passed.
- `python3 tools/validation/check-msc-elf.py build/validation-pico2-SDIO_PIO/pf_hello_world.elf build/validation-pico2-SPI/pf_hello_world.elf` — both existing artifacts passed wrapper-pointer positive and negative checks. SHA-256: SDIO_PIO `8e26a03bdc56ef50df7877993e9e20463d21a8ab4de1b58904fa49c723b20d32`; SPI `c2ee0ecf0432a29a278508e68d1234c82c96a37488ec979a4e358976a7fb0fa5`.
- `git diff --check` and `git diff --cached --check` — passed.

No firmware build, Ceedling run, physical hardware/HIL test, media operation, or performance benchmark was performed. No performance gain is claimed. No commit or push was performed.


## Publication state and follow-up review boundary

This note's first validation session predates publication: its candidate was subsequently committed and pushed as `6e9250dcc5f752fb8aadc22bb225ca9813b10365` on `performance-enhancement`. The earlier "No commit or push was performed" sentence applies only to that historical session. This follow-up remediation is a separate candidate against `6e9250d`; it remains uncommitted and unpushed, and has no final commit hash. Local refs were checked on 2026-10-15 at 17:56:25 UTC and then resolved to that baseline; the coordinator confirmed the push at that time. Later remote changes are outside this record.

## Follow-up fixes and remaining limits

1. **Fixed:** historical commit/push sentence is explicitly time-scoped above;
   final follow-up hash remains a coordinator responsibility.
2. **Clarified, exclusive phases remain unsupported:** existing profile ABI,
   CLI labels and historical values are retained as inclusive windows. The
   [measurement boundary table](../../sdcard-design.md#profile-measurement-boundaries)
   covers CMD24 response/busy, CMD25 encoding overlap, RX decode overlap,
   STOP command/busy and recovery nesting. They cannot be summed or directly
   compared across transports. The focused production CMD25 regression asserts
   deterministic DATA=119 and CRC/copy=51 ticks with nested encoding; test-seam
   verification is excluded from instrumentation. Existing CMD24 DMA+PIO,
   timeout, abort and mode-restore coverage remains. No hardware timing inferred.
3. **Fixed artificial callback restriction; current BOT ceiling remains:**
   complete sector multiples at offset zero now reach the batch backend; partial
   calls remain single-sector RMW and valid zero-byte calls perform no I/O.
   Signed return range, LUN, pointer, offset and capacity boundaries are checked.
   The recursive facade lock spans validation/RMW/completion. Multi-block success
   checks sync/STOP before returning; failed writes are never replayed or synced
   as recovery. Existing buffers, ownership and sync/eject contracts are retained.
   Pinned TinyUSB stages at most 512 bytes per current callback, despite multi-
   sector SCSI commands. No cross-callback write aggregation is added: its
   completion hook runs after CSW is queued and cannot report a final flush error.
   `check-msc-batch.py` executes production MSC + facade + diskio in both transport
   configurations with real byte-bearing backend seams, comparing all batch
   bytes, unaligned input/output, output guards, partial RMW preservation, ranges,
   ownership denial, capacity/sector size, sync failure/sense and no replay.
   This proves delivery to the backend seam, not USB multi-sector delivery or
   physical CMD18/CMD25 transactions. Existing transport host tests exercise
   production routing/encoding/control flow separately.
4. **Unresolved validation gap:** no new full BOT integration or HIL result.
   The pinned SDK unit-test project uses vendored Ceedling 0.31.1; checking its
   runner with `ruby vendor/ceedling/bin/ceedling version` fails before tests with
   `undefined method exists? for class File` under installed Ruby 3.3.8 (exit 1).
   An installed Ceedling 1.1.10 library does not make that configured project
   runnable without migration. No dependency installation, runner migration or
   vendor edits were made. Real-stack application sense/reset/short-transfer/CSW
   integration and USB-to-card busy/completion remain unverified. The review's
   GIL/integration concern is a validation gap, not a confirmed firmware bug;
   these C tests provide no GIL evidence.
5. **Fixed conditional error propagation; hardware boundary remains:** SPI
   deselect raises CS before the trailing-byte checked exchange and propagates
   failure through command/read/write/sync paths, invalidating readiness. Even a
   complete data write followed by trailing-exchange failure returns I/O error;
   it is never recovered or replayed. The production seam regression first failed
   on the old success return and now passes for CMD24, CMD25 and sync. The pinned
   SDK `spi_write_read_blocking` loops until all requested RX/TX bytes finish and
   returns `len`; `spi_write_blocking` additionally waits for BSY to clear.
   Neither has a normal short-return path. CS high is established in source and
   in the seam; physical SCK completion after an alternate failing SDK is unknown.
   Configured SPI CPOL=0 means idle SCK is low, not high; do not confuse SCK with
   deselect CS. No short-return hardware fault or physical clock state is claimed.

Existing CMD18/CMD25, TX double-buffer preparation, RX completed-block decode,
fused-copy options, preallocation and interrupt wait remain intact. No speculative
helper, clock change, buffer expansion or performance claim was added. A controlled
measurement recipe is documented in the design; it was not executed.

## Follow-up validation actually run

All twelve `tools/validation/check-*.py` entry points passed (exit 0):
`check-msc-batch`, `check-sd-msc-production`, `check-sd-serialization`,
`check-sd-spi-read-errors`, `check-sd-spi-transfer`, `check-sdio-init`,
`check-sdio-pio-timing`, `check-sdio-read-batch`, `check-sdio-tx`,
`check-storage-automount`, `check-validation-integrity` (2 tests), and
`check-msc-elf`. The ELF check inspected the **existing historical artifacts**
listed above with the same SHA-256 values; those are not builds of these edits.
PIO timing is still a model only. New MSC and SPI seam regressions first triggered
old behavior (multi-sector rejection / hidden trailing failure), then passed after
fixes; no source-token matcher was added for the batch byte contract.

`python3 -m py_compile` on all twelve validation entry points plus `msc_elf.py` passed; bytecode was directed to `/tmp/picoforge-review-pycache`. Host C regression compilation uses
`-Wall -Wextra -Werror`; the new diskio-inclusive harness locally suppresses only
its existing always-true uint32 width-guard warning, without changing diskio.
All three `git diff --check`, `git diff --cached --check` and
`git diff HEAD --check` passed; tracked, staged and untracked state was inspected.
Only the listed source/test/docs edits and two intentional new test files remain. The agent did not generate firmware; the coordinator subsequently rebuilt the existing ignored build directories as recorded below. Python bytecode cache is ignored.

Complete outputs and final inspection/result notes:
`/home/home/.hermes/cache/scratch/picoforge-sd-review-20261015T175643Z-2fdcbff6.log`.
The agent did not compile firmware or operate hardware, access media, flash, commit, push or create a PR. The coordinator then completed firmware builds and host unit tests as recorded below.

Final workspace note: SPI explicit invalidation and read trailing-error test
additions appeared concurrently after the first final snapshot. They were
preserved; `check-sd-msc-production`, `check-sd-spi-transfer`,
`check-sd-spi-read-errors` and `check-storage-automount` were rerun on those files
and passed (exit 0), followed by all three diff checks and state/diff inspection.
The log contains both snapshots and the later production SPI read/write output.

## Coordinator final validation

- Re-ran eleven argument-free `check-*.py` checks: all passed, including the new MSC batch regression in both SDIO and SPI configurations. The twelfth check, `check-msc-elf.py`, requires explicit ELF inputs; an argument-free invocation correctly exited 2 with usage, then the explicit dual-ELF invocation passed.
- `py_compile` for the twelve entry points and `msc_elf.py`, and worktree/index/HEAD whitespace checks passed.
- Built `pf_hello_world` in `build/validation-pico2-SDIO_PIO` and `build/validation-pico2-SPI` using their existing CMake configurations: both returned 0 and linked the changed firmware.
- Checked both rebuilt ELF driver/wrapper pointers, including negative checks: passed. New SHA-256 values are `3507cc109d92303ad4be526839eda4da47c74a6e9452845063df80b03343abd1` (SDIO) and `e5739bbfe111d609d3bc70301e76950d42eeb1dae3060a2b3643b54a5a606774` (SPI). These hashes identify bytes, not source or transport provenance on their own.
- `/home/home/.local/share/gem/ruby/3.3.0/bin/ceedling test:all` used installed Ceedling 1.1.10 for the repository's root `project.yml`: 122 passed, zero failures, zero ignored. This does not repair the separate vendored TinyUSB test runner incompatibility or establish BOT integration.
- No hardware/media/flash activity, dependency installation, commit, push or PR. Actual USB BOT/reset/fault behavior and controlled performance measurements remain outstanding; current 512-byte TinyUSB staging still prevents multi-sector callbacks in normal BOT operation.
