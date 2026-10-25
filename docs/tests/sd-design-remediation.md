# SD design review disposition

This records source remediation of the review of
`cc2816d6e5cf94a83dfc127bffd4dba5401f8fdb`, not target qualification.
The review confirmed no new production defect. The following independently
traced findings have different evidence requirements; passing host tests does
not turn conditional risks into demonstrated target failures or fixes.

| Finding / recommendation | Disposition and evidence |
| --- | --- |
| Synchronous MSC blocks the USB task during SD I/O, sync and recovery | Conditional risk retained. IRQ wait releases CPU but does not dispatch BOT/HID/CDC on that task. Measure callback/lock p95/p99 and worst case, plus HID/CDC service gaps, before changing architecture. No service deadline has yet been supplied or measured. |
| Worker busy/self-test/presence/automount state | Local improvement: busy completion, cancellation checks and initial self-test admission now use the existing critical-section rule; cancellation publication is atomic. Cancellation keeps the reservation until completion. Existing debounce/retry/removal/recovery behavior remains covered. Distinct lifecycle flags are retained because they encode different transitions, including intentional unmount and blocked recovery remount. |
| Filesystem/ownership handoff relies on workflow; unmount is not flush | Conditional API constraint documented in storage and MSC headers. FatFs remains worker-only (`FF_FS_REENTRANT=0`, static LFN); close/sync results must be checked before unmount/handoff. MSC task excludes worker busy/mounted media and reserves ownership changes. Removed/faulted cleanup is an explicit exception, never a successful flush. No new forced sync of absent media. |
| SCSI sync/eject/error contract with pinned TinyUSB | Local fix: failed START STOP for invalid LUN/disabled MSC sets fresh NOT READY sense instead of leaving stale sense. Shared pre-CSW sync helper preserves eject/cache error mapping. Added production-callback regressions for reserved fields/data length, NOT_CONFIGURED, failed eject retaining state and completion performing no sync. Existing tests cover prevention, range, owner handoff and transfer sense/reset. |
| 512-byte endpoint staging vs callback batching | Confirmed limitation retained. Strengthened pinned-source contract checks for staging cap, actual received WRITE length, built-in START STOP/PREVENT error ordering, generic no-data SCSI callback and post-CSW completion. Multi-sector facade/callback support does not establish end-to-end MSC batching. |
| Real BOT short input/reset/abort/CSW/error chain | Hardware/integration evidence blocked. Callback tests and source contracts do not execute the real BOT state machine. Qualification must observe status/residue/sense and card completion together. |
| Stack high-water and static resources | Measurement required. Overflow checking is configured, but high-water APIs/runtime stats and a reporting interface are not configured. No dead watermark instrumentation added. Worker 8 KiB / USB 4 KiB are allocations, not measured headroom. Native RX 33,280 B + CRC tables 8,192 B + self-test 32,768 B = 74,240 B before other static resources; 520/1,040 B wire stack images and init payload can overlap call frames. |
| Repeated recursive locks / init LBA0 copy | Retained: recursive locking is needed for composed facade reservations. New unlocked public paths would weaken boundaries. Verify-only init reuse needs byte/CRC/stop equivalence and stack evidence; no demonstrated gain justifies changing it here. Small safe duplication removal is limited to MSC sync error mapping. |
| Current-SHA qualification matrix and historical docs | Historical records retain their original SHA/config and are not current qualification. Validation of the working diff is recorded separately in the repair result. |
| fused-copy × polling/IRQ 2×2 | Target matched measurement blocked; defaults unchanged. Recipe below. Measured DATA/CRC/STOP profile scopes overlap and must not be summed; RECOVERY is reserved/unmetered. Historical 40.93% is CMD25 persistence A/B, not a fused-copy/IRQ winner. Historical CRC failures do not establish a current sampling defect. |
| Larger staging, async worker, zero-copy/chained DMA | Deferred design options requiring quantified benefit and BOT/HIL. Async needs an agreed maximum USB service gap/callback deadline, measured violations, persistent buffer/request lifetime, cancellation/reset handling and error delivery before CSW. No target deadline exists, so none is invented. Native wire coding constrains zero-copy. |

## Historical working-diff validation (cc2816d)

The remediation was implemented with Codex CLI and its actual source/test/doc
changes were inspected by the coordinator. These results apply to the uncommitted
working diff based on `cc2816d`, not to the unchanged baseline commit alone:

- Ceedling full suite: 38 suites, 298 tests, zero failures and zero ignored;
  the coordinator reran `test:all` successfully (exit 0).
- Pinned TinyUSB source contract: one test passed on coordinator rerun (exit 0).
- `pf_hello_world` compiled and linked for Pico 2 SPI, Pico 2 SDIO PIO and
  RP2040 Pico SPI. The experimental Pico 2 SDIO IRQ-wait configuration also
  compiled and linked; its build cache was restored to IRQ OFF, and the
  coordinator rebuilt that restored configuration successfully.
- `git diff --check` passed. No commit, push, dependency installation, firmware
  flashing or physical-media/USB test was performed.

Host callback regressions and source contracts do not execute the real BOT
state machine or establish target timing, stack headroom or performance gains.
The unresolved/deferred rows above remain unresolved despite these passes.

## Measurement recipe — future explicit hardware authorization required

No step here was executed on media or USB hardware during remediation.
Use the same identified board/card/host, firmware source plus diff hash,
SDK/toolchain, clock, voltage, wiring, filesystem layout, endpoint staging and
CMD25 policy for all four cells. Record those values and ELF/UF2 hashes. Change
only `PF_SDIO_PIO_FUSED_COPY={OFF,ON}` and
`PF_SDCARD_INTERRUPT_WAIT={OFF,ON}` in four separate build directories, with
`PF_SDCARD_TRANSPORT=SDIO_PIO` and `PF_SDCARD_PORT=FREERTOS`.
Do not select performance defaults from host builds.

After authorization, use an expendable card and explicitly approved test region
or file; record command transcript, dataset, sizes, repetitions and cleanup.
Run at least five repeats per cell, alternating/reversing cell order, and retain
all failures. Separately measure raw reads/writes, normal file and preallocated
file reads/writes, and host MSC traffic. Existing self-test is a normal-file
benchmark; preallocation/raw measurements need an approved harness. Use existing
`storage status` and self-test results as baseline observables (driver phase profiling was removed);
callback/lock timing, CPU and stack high-water still need a qualified target
capture interface. Report throughput mean/spread, p95/p99/max callback and sync
latencies, lock waits, HID/CDC service gaps, CPU and USB/worker stack minimum
headroom, CRC/busy/stop/recovery errors, byte verification and backend block count.
Retain time-correlated traces rather than adding overlapping profile intervals.

Before selecting a winner or changing staging/clocks/CMD25/defaults, execute real
BOT short IN/OUT, reset/abort, CSW status/residue, REQUEST SENSE after read/write
and sync failure, PREVENT/ALLOW, failed/successful eject, disconnect/removal and
recovery. Verify no failed-write replay, no success before checked completion,
and no post-CSW flush used as an error-reporting boundary. Publish a per-case
pass/fail/blocked matrix tied to the artifact and card. Changes to staging need
new SRAM accounting and observed end-to-end backend batching; hardware electrical
qualification remains separate from C/Python regression results.

## Codex source follow-up (8419b1f baseline)

This uncommitted follow-up starts at tidyup HEAD/origin/tidyup
`8419b1fc6b7b4c59663962b1b7f37183365556dd`. The earlier validation above is
historical and does not qualify this diff.

- SPI CMD18 CRC/payload failure now invalidates initialization even when CMD12,
  ready and deselection succeed, matching single-block read failure. Stop is
  still attempted; failed reads/writes are not replayed. Regression coverage
  checks exact mismatched wire CRC, successful stop, payload and stop failures,
  absence and removal between blocks, and NOT_READY without later bus access.
  Existing explicit write-rejection completion behavior is retained.
- RECOVERY/RECOVERY_ATTEMPT have no production instrumentation callers.
  Native stream cleanup (idle/CMD12), ordinary initialization and explicit worker
  fault recovery are distinct boundaries, not a defined shared recovery metric.
  Both transports therefore retain reserved/unmetered enum/array slots.
  Enable/reset/get and retained failure snapshots retain their contracts.
  CLI field names/order remain, but recovery numeric values now say `reserved`;
  numeric-only consumers must handle this explicit unsupported value. Command
  regressions cover profile and SPI/native diagnostics; shared profile tests
  check enum values and reset/get slots. No ordinary init is renamed recovery.
- Recursive facade reservation and worker busy gating remain. USB latency,
  stack headroom, actual MSC batch size and fused-copy × polling/IRQ 2×2
  measurements were not executed and still require target measurement. No
  buffer/staging, async interface, IRQ/clock/DMA default or architecture change.

Actual local verification by Codex (coordinator rerun still independent):

- RED: `ruby -e 'gem "ceedling"; load Gem.bin_path("ceedling","ceedling")' -- test:sd_spi test:commands`
  exited 1: SPI 30 tests/2 expected failures (CRC and payload initialized latch),
  commands 32 tests/3 expected failures (profile and diagnostics reserved output).
- GREEN: `ruby -e 'gem "ceedling"; load Gem.bin_path("ceedling","ceedling")' -- test:all`
  exited 0: 38 suites, 302 tests, zero failures/ignored.
- `/usr/bin/python3 -m pytest tests/contracts/test_tinyusb_msc_contract.py -q`
  exited 0: one test passed.
- `cmake --build build/<dir> --target pf_hello_world --clean-first --verbose -j 2`
  exited 0 for `validation-pico2-SDIO_PIO`, `validation-pico2-SPI` and
  `validation-pico-SPI`. Verbose logs showed respectively 552/546/558 compile
  commands and fresh ELF links. SDIO compiled native/native_host; SPI targets
  compiled sd_spi; all compiled current sd_protocol and storage commands and
  consumed sd_block.h. Cache SHA-256 values matched before/after exactly.
- `git diff --check` and staged diff checks passed; index remained empty.
  All commands ran from repository root. No dependencies installed, vendored
  sources changed, commit/push/PR, flashing, physical-media access or benchmark
  was performed. No validation blocker remained.
