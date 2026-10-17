# External NOR Flash

## Implemented Scope

The initial raw driver supports the W25Q128 JEDEC ID `EF 40 18`, 16 MiB
capacity, 256-byte program pages and 4 KiB erase sectors. It provides
identification, reads, page-split programming, aligned sector erase, readiness
polling and readback verification of program/erase operations.

Quad enable uses the W25Q128JV-compatible `35h` / volatile-status-enable `50h` /
status-register-2-write `31h` sequence. Verify the actual part marking and
datasheet before using quad mode: a JEDEC ID alone does not identify every
revision's status-register command behavior. Other NOR parts, QPI mode,
four-byte addressing and SFDP discovery are not supported.

This is not yet a FatFs backend. An optional LevelX NOR port provides logical
sectors and upstream reclamation/wear management, but filesystem mounting and
USB MSC export remain unimplemented. Raw programming requires previously
erased bits; it never erases implicitly. A failed write can leave partial changes.

## Layers and Wiring

- [Common NOR protocol](../firmware/src/drivers/nor_flash/nor_flash.h) owns
  device commands, geometry, write-enable checks, status and operation deadlines.
- [Transaction interface](../firmware/src/drivers/nor_flash/nor_flash_transport.h)
  supports single-lane command/address/dummy phases and single- or quad-lane data.
- [SPI adapter](../firmware/src/drivers/nor_flash/spi/nor_spi.c) uses the
  [shared SPI engine](../firmware/src/drivers/storage/spi/storage_spi.c), also
  used by SD-SPI.
- [QSPI-PIO adapter](../firmware/src/drivers/nor_flash/qspi_pio/nor_qspi_pio.c)
  uses the [physical quad-SPI engine](../firmware/src/drivers/storage/qspi_pio/storage_qspi_pio.c).
  The NOR protocol selects single-lane initialization and `6Bh` quad reads;
  programming remains single-lane `02h`. The physical engine does not identify
  NOR parts, set QE, or decide when to erase.
- [Board integration](../firmware/src/board/board_nor.c) selects GP2 clock,
  GP3..GP6 IO0..IO3 and GP7 CS, matching
  [the reference storage wiring](../firmware/src/drivers/storage/README.md#w25q128-flash-breakout).

The authoritative [physical storage-bus architecture](../firmware/src/drivers/storage/README.md#physical-storage-bus)
defines transport ownership, lifecycle, and shared-pin restrictions. Existing
NOR APIs remain available through the adapters; moving the physical engines
does not add FatFs integration or simultaneous SD/NOR access.

The board configuration starts at 1 MHz. The transport APIs reject requested
clocks above 10 MHz; that limit is not a hardware qualification. PIO divides
the actual system clock for four instructions per symbol, with one high-clock
instruction. CPU/FIFO pacing introduces additional low-clock gaps, so the
requested frequency is a maximum symbol cadence, not sustained throughput.
Both implementations use bounded polling rather than DMA. The QSPI transport
claims one state machine and eight instructions on PIO0 or PIO1, and restricts
its clock/data pins to GPIO0..31 on both MCU families.

Keep external CS/WP/HOLD pull-ups as documented. IO2/IO3 are inputs in the PIO
single-lane phase and data signals in quad mode. SPI holds WP/HOLD high.
Never hard-tie those pins to 3V3 when using quad transfers.

All contexts are caller-owned and must remain alive for the transport/device
lifetime. Zero-initialize transport contexts, serialize all access and dedicate
the selected pins/controller to this driver. Deinitialize before reinitializing.
A bus timeout deinitializes the transport; recover by reinitializing transport
and flash. The board wrapper owns its static transport context.

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
not the default SD/USB application's `pf_hello_world.uf2`. The existing
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
ceedling test:test_nor_flash
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

**Open is not read-only.** Upstream LevelX can initialize metadata on blank
media and repair/reclaim existing metadata during open. There is intentionally
no automatic open in board startup and no destructive erase-all/format wrapper.
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

The next storage step is adapting these logical sectors to FatFs and its
storage-owner lifecycle without changing SD behavior. The default SD/FatFs
application is unchanged by this port.
