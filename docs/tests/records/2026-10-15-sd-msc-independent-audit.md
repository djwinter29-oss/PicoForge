# Independent SD / MSC follow-up audit

Base: `performance-enhancement`, `06a12fe0e557dd4fb9ff25ad805f155b608df582`.
Workspace-write sandbox remained in place. No hardware access, commit, or push.
The prior `2026-10-15-sd-msc-review-host.md` record is preserved unchanged.

## Confirmed omissions fixed

- CMD24 command transfer/R1 timeout previously returned I/O error while retaining
  initialization, although the card could already be waiting for data. It now
  invalidates initialization, matching CMD25 command uncertainty. Confirmed R1
  rejection still fails without replay.
- One-byte SPI exchanges discarded the SDK transfer count. An untouched 0xff
  receive byte could falsely satisfy ready. Ready, command response (including
  CMD12 stuff byte), write CRC bytes, write response, and CMD25 stop transfer now
  check the count and fail on a short exchange. Initialization is invalidated;
  existing CMD25 cleanup still runs and cannot turn a failed write into success.
- SYNCHRONIZE CACHE(10) now validates LBA/count against capacity before whole-card
  sync. Count zero means the remainder of the medium; out-of-range LBA and ranges
  fail with ILLEGAL REQUEST / LBA OUT OF RANGE, without backend sync.
- PREVENT values other than supported nonpersistent 0/1, and unsupported control
  values, fail with INVALID FIELD IN CDB without mutating prevention state.
- Repeating an allowed eject succeeds without another sync. Loading uninitialized
  media fails without clearing ejected state. PREVENT still takes precedence.
- If removal/availability changes after the initial readiness check, backend
  NOT_READY / NOT_CONFIGURED from sync or eject now reports NOT READY rather than
  MEDIUM ERROR. Backend I/O failure still reports MEDIUM ERROR and leaves eject
  state unchanged.

## Findings retained after contract review

TinyUSB's in-tree `msc_device.c` passes non-built-in commands to `tud_msc_scsi_cb`;
zero is a successful no-data response and negative fails. WRITE10-complete is
invoked after status processing; it cannot report a sync failure. MSC full-sector
writes call either transport with count one. SPI checks data response and busy;
SDIO's CMD24 path checks response/busy and invalidates on uncertain completion.
No application write cache exists. Explicit sync/eject and owner-release sync
remain necessary checked boundaries. Ownership release requests reject PREVENT,
busy workers, and unejected present media; failed handoff restores enabled state.
USB callbacks, HID requests, and MSC transition application run in the USB task;
the facade uses a recursive card reservation for cross-task I/O and handoff.

FatFs `f_open` implements FA_CREATE_NEW atomically with FR_EXIST for an existing
entry. The self-test only unlinks after successful creation and successful close.
FatFs `f_close` first syncs and invalidates its object only on successful close;
retaining a file after close failure is appropriate. Existing-name/open failures
perform no unlink. Two additional regressions cover preallocation plus failed
close, and failed read close after full verification. No self-test implementation
change was justified; all integrity checks and fixed-name collision refusal stay.

The profile's 20 MHz request is conservative, not evidence of a 25 MHz runtime
bus. Production slow-mode divider rounding at 150 MHz produces divider 4 and an
18.75 MHz bus. The comment now states that calculation. SDIO initialization
continues to probe transfers and reinitialize at successively lower ceilings;
CMD6 negotiation, CMD12 recovery, CRC, DMA bounds/barriers, and PIO/DMA completion
checks were retained. Signal-integrity causation and physical init reliability
cannot be established from host execution or a firmware build.

## Performance decisions

Retain the prior removal of redundant WRITE10-complete sync: the production
regression proves no extra backend sync call. This is a call reduction, not a
measured throughput gain. There is no further proven optimization in this audit.
Leave clock increases, fused copy, interrupt waits, preallocation, SRAM placement,
CRC-table changes, and batch-size changes untouched: benefit depends on hardware
phase timing, contention, card allocation, and identical file-level benchmarks.
The candidate uses fused-copy OFF, interrupt-wait OFF, preallocation OFF. Explicit
self-test sync/close and byte-for-byte verification remain unchanged. Historical
25 MHz throughput records do not describe this 18.75 MHz candidate.

## Validation observed

- Complete production SPI and MSC regressions: PASS, `-Wall -Wextra -Werror`.
  Added command/token/CRC/response/busy/stop short-transfer and no-response cases,
  range validation, invalid PREVENT fields, repeated eject, absent load, and
  backend NOT_READY checks.
- Storage/MSC lifecycle, concurrent facade serialization/removal/handoff, SDIO
  initialization/routing and copy/interrupt race checks, production SDIO TX and
  command/stop recovery, SDIO read batching, SPI batching: PASS.
- Assembled SDIO PIO timing: PASS (host model only).
- Installed Ceedling 1.1.10 invoked directly:
  `/home/home/.local/share/gem/ruby/3.3.0/bin/ceedling test:all`: 122 tests,
  zero failures/ignored. Log: `/tmp/picoforge-review-unit.log`.
- `cmake --build build/copilot-review-candidate --parallel 2`: PASS;
  Pico2 / rp2350-arm-s / Release / SDIO_PIO / 150 MHz. Linked `pf_hello_world.elf`.
- `cmake --build build/validation-pico2-SPI --parallel 2`: PASS;
  also compiles the changed production SPI backend and links firmware.
- Shell syntax and `git diff --check`: PASS.

The aggregate `validate.sh --skip-firmware` reached all host regressions but its
coverage environment wrapper attempted to create `/home/home/.cache/uv/.tmp...`
and failed with read-only filesystem (os error 30). No sandbox bypass or install
was attempted; the already-installed Ceedling was run directly. Initial harness
compilation failures from missing new helper/constants were corrected in test
seams and those checks rerun successfully. Aggregate log:
`/tmp/picoforge-review-validation.log` (includes wrapper failure).

Changed paths:
- `firmware/src/board/board_profile.c`
- `firmware/src/drivers/sd_card/spi/sd_spi.c`
- `firmware/src/usb/msc.c`
- `firmware/tests/platform/storage/test_self_test.c`
- `tools/validation/check-sd-spi-transfer.py`
- `tools/validation/check-storage-automount.py`
- `tools/validation/host/regression_msc.c`
- `tools/validation/host/regression_spi_write.c`
- `docs/tests/records/2026-10-15-sd-msc-independent-audit.md`

Remaining physical validation: card init stability, signal timing, USB host eject
interoperability, power-loss behavior, and comparable throughput. No physical
validation was performed or inferred. Temporary compiler/build outputs were
created only in permitted workspace/scratch locations.
