# External NOR Flash

## Implemented Scope

The initial raw driver supports the W25Q128 JEDEC ID `EF 40 18`, 16 MiB
capacity, 256-byte program pages and 4 KiB erase sectors. It provides
identification, reads, page-split programming, aligned sector erase, readiness
polling and readback verification of program/erase operations. Initialization also reads the factory-programmed
64-bit Unique ID using `4Bh`, 32 dummy clocks and eight single-lane data bytes. This read does not program or erase
flash. UID transport failures fail initialization explicitly; the UID is not an authentication credential.

Quad enable uses the W25Q128JV-compatible `35h` / volatile-status-enable `50h` /
status-register-2-write `31h` sequence. Verify the actual part marking and
datasheet before using quad mode: a JEDEC ID alone does not identify every
revision's status-register command behavior. Other NOR parts, QPI mode,
four-byte addressing and SFDP discovery are not supported.

An optional LevelX NOR port provides logical sectors and upstream reclamation/wear management.
The application can select these sectors for FatFs and USB MSC at build time; SD remains the default.
Raw programming requires previously erased bits; it never erases implicitly. A failed write can leave partial changes.

## Layers and Wiring

- [Common NOR protocol](../firmware/src/drivers/nor_flash/nor.h) owns
  device commands, geometry, write-enable checks, status and operation deadlines.
- [Transaction interface](../firmware/src/drivers/nor_flash/nor_transport.h) and
  [common adapter](../firmware/src/drivers/nor_flash/nor_transport.c) use the same storage-bus handle for SPI
  and QSPI-PIO. Transactions contain single-lane command/address/dummy phases and single- or quad-lane data.
  NOR owns this transaction structure. The adapter validates address width and payload size, constructs big-endian
  opcode/address bytes, then submits raw phases through [the serial capability](../firmware/src/drivers/storage/serial.h).
  It also maps physical results and supplies the protocol's microsecond clock.
- Physical signaling uses the [shared SPI engine](../firmware/src/drivers/storage/spi/spi.c), also used by
  SD-SPI, or the [quad-SPI engine](../firmware/src/drivers/storage/qspi_pio/qspi_pio.c).
  The NOR protocol selects single-lane initialization and `6Bh` quad reads; programming remains single-lane `02h`.
  Neither physical engine identifies NOR parts, sets QE, or decides when to erase.
- [Board profile](../firmware/src/board/board_profile.c) selects GP2 clock,
  GP3..GP6 IO0..IO3 and GP7 CS, matching
  [the reference storage wiring](../firmware/src/drivers/storage/README.md#w25q128-flash-breakout).

The authoritative [physical storage-bus architecture](../firmware/src/drivers/storage/README.md#physical-storage-bus)
defines transport ownership, lifecycle, and shared-pin restrictions. Sharing the adapter does not add FatFs
integration or simultaneous SD/NOR access.

The protocol and common adapter live together in `firmware/src/drivers/nor_flash/`:

```text
nor_flash/
  nor.c / .h            NOR protocol, geometry, programming and erase
  nor_transport.c / .h  Common storage-bus adapter and result translation
```

Production and unit-test filenames use the short `nor` prefix. The device type is `pf_nor_t`, and protocol entry
points are `pf_nor_init`, `pf_nor_read`, `pf_nor_program`, `pf_nor_erase`, `pf_nor_sync`, and `pf_nor_enable_quad`.
These replace the former `pf_nor_flash_*` type and functions; callers must migrate without compatibility aliases.
Transport/result names already use `pf_nor_*` / `PF_NOR_*` and are unchanged. Build targets, options, and upstream
LevelX `lx_nor_flash_*` names retain their existing names.

`pf_nor_transport_init` replaces the former `pf_nor_spi_init` / `pf_nor_qspi_pio_init` entry points. Pass a persistent,
configured `pf_storage_bus_t` handle and a transport output; the common adapter initializes the bus and derives quad
capability from its serial operations. Native SD handles are rejected. The handle and concrete context must outlive the flash
device. Deinitialize with `pf_storage_bus_deinit` after device operations finish. NOR protocol and LevelX APIs are
unchanged; direct callers of the former bus-specific adapters must migrate.

The former NOR-specific board wrapper is removed. The
[board storage interface](../firmware/src/board/board_storage.h) prepares the selected physical descriptor and
board-specific idle pins without initializing the bus or referencing NOR. The standalone smoke follows this sequence:

1. `pf_board_storage_prepare(&bus)` configures board-owned runtime state from the immutable profile and prepares
   WP/HOLD for SPI. It acquires no SPI/PIO resources.
2. `pf_nor_transport_init(&bus, &transport)` initializes the bus; `pf_nor_init` identifies the flash.
3. After all operations, call `pf_storage_bus_deinit(&bus)`, then `pf_board_storage_release_pins()`.
   Release prepared pins even if subsequent bus/device initialization fails.

`PF_BOARD_STORAGE_QSPI` selects the serial board profile for the standalone target; CMake derives it from
`PF_NOR_TRANSPORT`. The SD application retains its existing profile and does not prepare the alternative serial device.

The board requests 40 MHz for NOR SPI and configures a 100 ms stalled-FIFO deadline. The Pico SDK chooses
a realizable baud rate no higher than requested: at the reference 125 MHz system clock this is 31.25 MHz,
not 40 MHz. HID telemetry reports the actual baud rate. The NOR board policy accepts SPI requests up to 40 MHz;
generic SPI also serves SD and has no NOR-specific clock ceiling.
QSPI-PIO still starts at 1 MHz and rejects requested clocks above 10 MHz.
Signal integrity at higher SPI clocks depends on the board, breakout and wiring; qualify with write/read verification,
not just successful identification. The bus clock is not the file throughput: polling, NOR program/erase verification,
LevelX and FAT all add overhead.
PIO divides
the actual system clock for four instructions per symbol, with one high-clock
instruction. CPU/FIFO pacing introduces additional low-clock gaps, so the
requested frequency is a maximum symbol cadence, not sustained throughput.
Both implementations use bounded polling rather than DMA. The QSPI transport
claims one state machine and eight instructions on PIO0 or PIO1, and restricts
its clock/data pins to GPIO0..31 on both MCU families.

Bounded SPI polling pipelines up to eight bytes, matching the hardware receive FIFO depth so interrupted transfers
cannot overflow RX. Timer reads occur only while neither FIFO makes progress, not once per payload byte.
The deadline is a maximum continuous stall interval; successful transfers still drain every transmitted byte.
SD-SPI's zero-timeout SDK bulk path is unchanged. NOR program/erase readback verification remains enabled.

Keep external CS/WP/HOLD pull-ups as documented. IO2/IO3 are inputs in the PIO
single-lane phase and data signals in quad mode. SPI holds WP/HOLD high.
Never hard-tie those pins to 3V3 when using quad transfers.

All contexts are caller-owned and must remain alive for the transport/device
lifetime. Zero-initialize transport contexts, serialize all access and dedicate
the selected pins/controller to this driver. Deinitialize before reinitializing.
A bus I/O error or timeout deinitializes the transport without replay. The caller owns the bus handle;
[board_storage.c](../firmware/src/board/board_storage.c) owns the concrete runtime context, separate from the immutable
[board profile](../firmware/src/board/board_profile.c). Recover by deinitializing the bus, releasing board pins, then repeating
preparation, transport initialization, and flash initialization.
WP/HOLD setup belongs to board support, not the common adapter. In SPI mode these pins remain driven high after a bus
transfer failure until board pin release; external pull-ups keep their idle levels afterward.

## Standalone Build

NOR and SD share the reference pins. The standalone target returns from CMake
before configuring the SD/USB/FreeRTOS application, so they cannot both start.
Use a separate build directory; do not attach both breakouts to the shared
pins. No hardware flashing is performed by these commands.

From the repository root:

```sh
tools/firmware/build.sh --board pico2 --nor-transport QSPI_PIO
```

Use `--nor-transport SPI` for the SDK SPI transport. These commands use separate
`build/nor-BOARD-TRANSPORT` directories and produce `pf_nor_flash_smoke.uf2`,
not the default SD/USB application's [named PicoForge image](../README.md#1-build-the-firmware). The existing
application name and default build behavior are unchanged.

Equivalent direct CMake configuration:

```sh
cmake -S firmware -B build/nor-pico2-qspi -G Ninja \
    -DPICO_SDK_PATH="$PWD/firmware/third_party/pico-sdk" \
    -DPICO_BOARD=pico2 -DPF_BUILD_NOR_SMOKE=ON -DPF_NOR_TRANSPORT=QSPI_PIO
cmake --build build/nor-pico2-qspi --parallel
```

Select `PF_NOR_TRANSPORT=SPI` and a distinct build directory for SPI0.
Use `PICO_BOARD=pico` for RP2040. Both transports support either board.
The output is `pf_nor_flash_smoke.uf2`.

The [smoke program](../firmware/tests/ports/nor_flash_smoke.c) reports its
numeric `pf_nor_result_t` on the SDK's UART (GP0/GP1 on Pico/Pico 2, 115200 baud).
It identifies the flash and reads the first 32 bytes. In QSPI mode it also
enables volatile QE and compares single/quad reads. It does not program, erase,
format or alter nonvolatile status registers. It loops after reporting so a
failure cannot be mistaken for ongoing testing. This smoke is not a wiring
or sustained-transfer qualification; matching blank reads alone does not
establish correctness across all data patterns.

## Tests and Remaining Qualification

```sh
ceedling test:test_nor
python3 -m pytest tests/pio/test_nor_qspi_timing.py
```

The C tests execute the protocol with a fake NOR array enforcing page boundaries,
write enable and 1-to-0 programming. They check range rejection, preserved
neighboring data, timeouts, quad-enable verification and ignored/protected writes.
The pytest model assembles the actual PIO programs and checks symbol width,
bit order, clock sidesets and blocking FIFO boundaries. Neither executes the
physical SDK transport on a board.

Board qualification remains required for CS timing, pin turnaround, actual
clock cadence, nonblank single/quad comparison and sustained reads. Destructive
program/erase testing must use an explicitly authorized disposable sector,
verify neighboring sectors, and never assume the attached flash is empty.

## LevelX NOR Port

[Eclipse ThreadX LevelX](../firmware/third_party/levelx) is pinned as a submodule
to `v6.4.5.202504_rel`, commit
`1751446fb0c1793cd60b05c5816305f2cdc3dd12`. The upstream
[MIT license](../firmware/third_party/levelx/LICENSE.txt) is retained unchanged.
Only common NOR engine sources are compiled; the upstream simulator, NAND,
FileX and ThreadX are not dependencies of this port.

```sh
git submodule update --init firmware/third_party/levelx
tools/firmware/build.sh --board pico2 --nor-transport QSPI_PIO --levelx
ceedling test:test_levelx_nor
```

`--levelx` enables `PF_BUILD_LEVELX=ON`, builds `pf_levelx_vendor` and
`pf_levelx_nor`, and links `pf_levelx_nor_link_smoke`. That executable exercises
only invalid-device rejection: it performs no NOR initialization, open, erase
or program. The separate raw smoke behavior remains unchanged. This is a
compile/link check, not a LevelX hardware qualification.

The [port API](../firmware/src/integration/levelx/levelx_nor.h) accepts a
caller-owned, initialized raw NOR instance and an explicit erase-aligned region.
It is transport-independent and uses either raw SPI or QSPI-PIO. Reads only use
quad transfers if the caller has successfully enabled quad mode on the raw
device; the port never changes QE itself.

Use a statically allocated, zero-initialized `pf_levelx_nor_t`, call
`pf_levelx_nor_open`, then use the read/write/sync/close functions. Each logical
sector is 512 bytes, with unaligned caller buffers supported by aligned staging.
The driver callbacks convert LevelX word-address tokens into bounded byte offsets
inside the supplied region. Tokens are never dereferenced; `LX_DIRECT_READ`
is forbidden. Two physical erase blocks are reserved from exported capacity.
For a full 16 MiB device this exports `(4096 - 2) * 7 = 28658` sectors,
14,672,896 bytes; obtain the exact capacity from `volume.sector_count` rather
than assuming all raw bytes are available.

LevelX runs in standalone mode: no ThreadX mutexes, interrupts or heap
allocation are added. Serialize all instances and operations in one storage
owner, including the first open/global initialization. The config fixes the
on-flash `ULONG` ABI at 32 bits on RP2040, RP2350 and 64-bit hosts. Context size
is `sizeof(pf_levelx_nor_t)` for the selected build; allow additional task stack
space for a 512-byte staging sector and upstream reclaim calls.
Each volume also owns an 8 KiB upstream cache, including the logical-sector mapping bitmap.
The bitmap avoids a whole-region search for every previously unwritten sector; it is rebuilt on open,
so it changes RAM use and startup work, not the on-flash layout. Cache initialization errors fail the open.
The upstream sector-mapping cache is configured for 4096 entries (48 KiB on RP2040/RP2350), rather than the
16-entry upstream default. This retains mappings for typical 1 MiB file tests without repeated metadata searches.
It is a bounded, four-way hashed cache, not a full-region map; larger/conflicting workloads can still miss.

**Open is not read-only.** Upstream LevelX can initialize metadata on blank
media and repair/reclaim existing metadata during open. The raw driver and link smoke do not automatically open media.
The NOR filesystem application does open its explicitly configured region in the storage worker; there is no
destructive raw erase-all wrapper.
Only supply an explicitly designated LevelX region, never an SD filesystem,
boot flash, or unrelated raw data. LevelX's on-flash layout is not FAT.

Failures return LevelX status. Raw errors and upstream system errors are retained
in `last_raw_result` and `last_system_error`, and latch `faulted` to reject further
I/O. After a media failure, close, recover/reinitialize the raw transport/device,
and reopen; failed writes can have committed and must not be blindly replayed.
Unwritten-sector results are returned as LevelX statuses, not silently converted
to zero-filled successful reads.

The Ceedling tests execute the real pinned NOR engine against a simulated
1-to-0 array. They cover rewrites, full logical capacity, reclamation with cold
data, subregion boundaries, reopen persistence, unaligned buffers, injected I/O
errors and partial program interruption. These bounded fault cases are not an
exhaustive proof of power-fail safety or endurance. Physical erase interruption,
real device timing and wear distribution still require dedicated qualification.

## Build-Selected FatFs and USB MSC

`PF_STORAGE_BACKEND=SD|NOR` selects the application filesystem and MSC backend; the default is `SD`.
NOR automatically builds the pinned LevelX port and
[logical adapter](../firmware/src/middleware/storage/levelx_block_device.h). `PF_BUILD_LEVELX=ON` also makes the
adapter available to SD application builds without selecting NOR or opening flash.
The [application NOR service](../firmware/src/app/storage/nor_media.h) owns initialization, a recursive FreeRTOS
reservation, and separate owner-gated firmware/USB descriptors. Raw NOR is never exposed directly as rewritable sectors.

NOR configuration requires **both** `PF_NOR_REGION_OFFSET` and `PF_NOR_REGION_SIZE`; neither has a default.
Values accept decimal or hexadecimal bytes, must be 4 KiB aligned, and must fit within the supported external
16 MiB W25Q128. The application requires at least 48 KiB: after LevelX's two reserved blocks this provides at least
FatFs's minimum 64 logical sectors. The region must be explicitly reserved for LevelX; do not use Pico boot flash
or a region containing unrelated raw data. Runtime initialization validates the detected part and region again.

After independently approving the external region, set its offset and size in your shell and build:

```sh
: "${PF_NOR_REGION_OFFSET:?Set the approved external region offset in bytes}"
: "${PF_NOR_REGION_SIZE:?Set the approved external region size in bytes}"
tools/firmware/build.sh --board pico2 --storage-backend NOR --nor-transport QSPI_PIO \
    --nor-region-offset "$PF_NOR_REGION_OFFSET" --nor-region-size "$PF_NOR_REGION_SIZE"
```

This produces the [named PicoForge image](../README.md#1-build-the-firmware) under
`build/firmware-pico2-NOR-QSPI_PIO`, not a raw smoke.
Use `SPI` for the supported single-lane transport, or `--storage-backend SD` for the existing SD workflow.
Without `--storage-backend NOR`, the existing `--nor-transport` option still selects the standalone raw smoke.
For direct CMake, pass the same `PF_STORAGE_BACKEND`, `PF_NOR_TRANSPORT`, and `PF_NOR_REGION_*` values.
The NOR filesystem/MSC application requires the FreeRTOS configuration.
When intentionally flashing later, `tools/firmware/load.sh` accepts the same backend, transport and region options.
It preserves NOR selection when invoking the build wrapper and rejects a known cached-backend mismatch before
flashing an implicitly selected ELF. `--skip-build` avoids rebuilding; an explicit `--elf` remains the caller's choice.

The worker normally opens LevelX and attempts to mount an **existing** FAT/exFAT volume.
For an unformatted logical volume, mounting reports a FatFs error; use `fs mount` to see the result.
After initialization, USB MSC can expose that unmounted logical volume for intentional host-side provisioning.
Host formatting is destructive to the logical region and must be explicitly authorized.
Alternatively, `fs format --confirm` creates FAT on the selected LevelX logical volume and mounts it.
This command requires Pico ownership and an unmounted volume (`fs unmount` first if necessary); it destroys
existing logical files but does not raw-erase the chip or access outside the configured LevelX region.
Formatting failure is reported and can leave partially changed filesystem contents.
Logical capacity excludes LevelX metadata/reserve space; USB reports that logical capacity.
Filesystem capacity is smaller again after FAT overhead. Reserve a sufficiently large region for file tests:
a 64 KiB region cannot hold a 1 MiB test; a 4 MiB region provides about 3.5 MiB of logical capacity.
Changing region geometry is not a filesystem resize. Previously unrelated contents in newly included blocks can
prevent LevelX open (`LX_SYSTEM_INVALID_BLOCK`); FAT formatting cannot repair invalid LevelX metadata.
Any raw re-provisioning must be separately authorized for the exact disposable region; the format CLI never
automatically erases raw contents.

The dashboards report NOR type, readiness, SPI/QSPI mode, effective bus clock, logical and physical capacity,
and JEDEC-derived Winbond/W25Q128 identity using
[NOR memory telemetry](usb-protocol.md#nor-memory-telemetry-input-report-3-layouts-3-and-4).
The NOR device serial displays the chip's factory 64-bit UID as 16 hexadecimal digits, not the Pico USB serial.
Revision and manufacturing date are unavailable and are not fabricated.
The same [file write/verify test](sd-self-test.md) supports NOR when mounted and Pico-owned, subject to free space.
Use a 1 MiB size for the 4 MiB region; the default 32 MiB test does not fit.

Only `LX_SECTOR_NOT_FOUND` reads map to erased `0xff` sectors; other failures remain errors. The lower-level LevelX API
continues to report its native statuses unchanged.

MSC enable rejects mounted or busy firmware access. Host reads/writes use only the USB descriptor; firmware operations
remain rejected while USB owns media. Eject and release check sync results; uncertain writes are never replayed.
Release publishes a worker remount request. Explicit recovery of faulted USB-owned media detaches USB and hands the
reservation to the worker before closing/reinitializing/reopening LevelX; this is not reported as a clean eject or
successful flush. Recovery leaves automount blocked until a successful explicit `fs mount`.

The application exposes named firmware/USB descriptor getters and explicit USB acquire/release operations.
MSC holds the media reservation across its worker-idle check and ownership transition; the NOR service checks
readiness and mounted state on acquisition, not worker admission. Normal release requires caller-checked eject/sync.
The separate recovery release rejects healthy media and does not sync or claim durability.

The separately confirmed [force-return action](usb-protocol.md#confirmed-force-return) supports healthy NOR as well
as SD without weakening normal Disable or fault-recovery checks. It disconnects before releasing ownership and
attempts one worker mount; host-cached writes can still be lost and the filesystem can be corrupted.

For compatibility, `sd status` and `sd diagnostics` report the selected NOR media with `backend=nor`, current
ownership/readiness and a retained failure sequence, block result, raw NOR result, LevelX result and system error.
An additional line reports `failure_operation` and `failure_owner`, independently of current ownership. Operation
codes are 0 none, 1 board preparation, 2 transport/flash initialization (including quad enable), 3 LevelX open,
4 LevelX close, 5 read, 6 write and 7 sync. Before any failure the owner is `none`. Successful recovery and rejected
owner I/O do not replace the retained failure. LevelX open/close retain their returned codes; logical block failures
use the synthetic `LX_ERROR` summary because the block adapter does not expose the original LevelX return code.
`LevelX_block_error=synthetic` labels that convention, not an additional failure.

Board validation checks common UART/LED resources and the selected NOR SPI or QSPI topology, ignoring unused SD
wiring. Serial preparation repeats that check before changing idle pins or publishing a bus descriptor. Every
selected signal must be unique, in GPIO range and disjoint from UART/LED, including SPI WP/HOLD; QSPI data lanes
must be consecutive. This validation does not acquire transport hardware or establish electrical compatibility.
Successful recovery does not clear the retained failure. Adapter I/O failures report `LX_ERROR`; initialization
and close failures retain the returned LevelX status. `sd info`, `sd profile`, and diagnostic clearing remain
SD-only. Recovery completion or failure is logged on UART, and leaves the volume unmounted in either case.
NOR builds omit SD protocol/execution-port targets and the middleware SD adapter, and board SD configuration
returns unavailable without preparing SD hardware.
The SD-specific HID memory report remains unconfigured rather than advertising NOR as an SD card.
Filesystem, MSC and file-self-test reports remain available. No new NOR identity/HID protocol is implemented.

```sh
ceedling test:test_levelx_block_device test:test_nor_media test:test_storage_worker_nor test:test_storage_commands_nor
ceedling test:test_usb_msc_nor test:test_levelx_nor
python3 -m pytest -q tests/contracts/test_storage_backend.py
```

The tests include real FatFs file create/write/sync/reopen/read/delete through the adapter and pinned LevelX engine
over simulated NOR, plus owner exclusion, batch/partial MSC transfers, failed-write non-replay, retry and recovery.
They are not physical NOR, USB BOT, endurance or power-loss qualification. No NOR filesystem hardware qualification
is claimed.
