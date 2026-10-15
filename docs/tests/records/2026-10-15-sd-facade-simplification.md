# SD facade simplification and validation record — 2026-10-15

Base: `performance-enhancement`, HEAD `aaab8b8769bb69dbe05cc7c7da01335bbb4b0d71`.

The facade simplification, CMD24 DATA/TIMEOUT profiling, expanded tests and original validation record were preserved. Follow-up work addresses two confirmed validation gaps; no firmware SPI behavior was changed.

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
