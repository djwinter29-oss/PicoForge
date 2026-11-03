# SDIO read throughput fix — 2026-10-13 UTC

## Scope and artifact

- Repository: PicoForge, branch `fix-sdcard`, base `0f000b2` plus uncommitted changes.
- Implementation/review backend: GitHub Copilot CLI; parent built, flashed and exercised hardware.
- Hardware: Pico 2 / RP2350; CPU 150 MHz, 4-bit PIO SDIO 12.5 MHz. Card capacity reported 7,969,177,600 bytes (15,564,800 sectors). Device/card unique identifiers omitted.
- Final Release firmware built for Pico and Pico 2; only Pico 2 physically exercised.
- Final Pico 2 ELF SHA-256: `8f7697c68a1ba9f4c28f1d137df3654792b4374179ca4a1b0729110aafd903cc`.
- Final Pico 2 UF2 SHA-256: `3d3209f4d188a9463c42416111bdc9891905b5942480d07a40c0ea852759f2b9`.
- OpenOCD SWD programming and verify returned `Verified OK` before final runs. No formatting, overclocking, committing or pushing.

## Bottleneck and changes

The original reader received a whole batch before lane decoding and four-lane CRC computation. This serialized transfer and CPU work. Controlled intermediate builds distinguish the effects:

- Fresh baseline, 32 MiB: write 7,503 ms / 4.472 MB/s; read 14,135 ms / 2.374 MB/s.
- CRC byte lookup: 32 MiB read 2.519 MB/s.
- Byte lane lookup and wire-order CRC: 32 MiB read 2.886 MB/s.
- 64-sector batches and 32 KiB self-test buffer: 32 MiB read 3.112 MB/s.
- Overlapping completed-block decoding with ongoing reception: approximately 4.70 MB/s.

The final helper observes DMA `write_addr`, not `transfer_count`, to identify fully written wire blocks, validates its range/alignment and applies a memory barrier before decoding. No CRC check was removed. A batch count above 32 is loaded via FIFO/pull/mov rather than the 5-bit PIO SET immediate. Cancellation and CMD12 cleanup remain.

Static storage growth is about 34.5 KiB (16 KiB test buffer, 16.25 KiB wire buffer, 2 KiB CRC lookup, 256-byte lane lookup plus a flag/padding). Final ELF BSS is 101,836 bytes; this is not a runtime stack/heap high-water measurement.

## Final hardware results

All rates below are decimal MB/s and include the existing filesystem/self-test measurement boundaries. Full pattern verification and CRC remain enabled. Raw terminal result summaries, actual bytes/times and intermediate results are in `2026-10-13-sdio-read-throughput.json`.

Final 128 MiB runs, each writing and reading 134,217,728 bytes:

- 22:01:56 UTC: write 26,672 ms / 5.032 MB/s; read 28,601 ms / 4.693 MB/s; host wall 55.521 s.
- 22:03:07 UTC: write 26,248 ms / 5.113 MB/s; read 28,583 ms / 4.696 MB/s; host wall 55.272 s.
- 22:04:17 UTC: write 25,964 ms / 5.169 MB/s; read 28,584 ms / 4.696 MB/s; host wall 54.776 s.
- All three: PASS, zero mismatches, error/filesystem/request error zero, file removed and volume unmounted.
- Sum of active host-observed wall durations: 165.569 s, excluding inter-run status acquisition gaps. This is not a long-duration soak test.

Final matched-size 32 MiB run at 22:05:27 UTC: write 6,610 ms / 5.076 MB/s; read 7,161 ms / 4.686 MB/s; wall 14.280 s. Read improvement over the fresh 32 MiB baseline is 97.39%.

Reading cancellation was exercised on the earlier overlapping build: cancelled after 2,228,224 read bytes, zero errors, file removed and volume unmounted. A subsequent 32 MiB run passed. This cancellation test predates the final change from transfer count to completed-write address; final cancellation is covered by host/unit checks, not repeated physically.

## Regression validation

- Firmware C unit tests: 106 tests, zero failures.
- Python host: 27 passed; WebHID: 16 passed; .NET: 14 passed.
- `python3 tools/validation/check-sdio-read-batch.py`: passed. Compiles the actual extracted C helper with DMA/decoder stubs and exercises completion boundaries, incomplete CRC tails, exactly-once decoding, invalid DMA addresses, barrier execution and injected CRC rejection; structural checks cover count loading and cleanup.
- Pico and Pico 2 Release builds passed; final Pico 2 flash verified.
- `git diff --check` passed. Intended changes are left uncommitted.

## Coverage limits

These are board-local FAT file benchmarks on one card and one Pico 2, not USB MSC throughput or continuous PIO sampling limits. No final 25 MHz run, power-loss test, physical CRC-error injection, card removal test, multi-hour soak or RP2040 hardware run was performed. The current implementation uses one bounded 64-sector buffer with decode/receive overlap, not a chained ping-pong streaming pipeline.
