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

This is not a FatFs backend. There is no NOR FTL, wear leveling, filesystem
mounting or USB MSC export. Raw programming requires previously erased bits;
it never erases implicitly. A failed write can leave partial changes.

## Layers and Wiring

- [Common NOR protocol](../firmware/src/drivers/nor_flash/nor_flash.h) owns
  device commands, geometry, write-enable checks, status and operation deadlines.
- [Transaction interface](../firmware/src/drivers/nor_flash/nor_flash_transport.h)
  supports single-lane command/address/dummy phases and single- or quad-lane data.
- [SPI transport](../firmware/src/drivers/nor_flash/spi/nor_spi.c) uses SDK SPI0.
- [QSPI-PIO transport](../firmware/src/drivers/nor_flash/qspi_pio/nor_qspi_pio.c)
  uses single-lane initialization and `6Bh` quad reads on the same PIO state
  machine. Programming remains single-lane `02h`.
- [Board integration](../firmware/src/board/board_nor.c) selects GP2 clock,
  GP3..GP6 IO0..IO3 and GP7 CS, matching
  [the reference hardware setup](hardware-setup.md#w25q128-flash-breakout).

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

The next storage step is selecting a reviewed NOR translation layer with
recoverable metadata and wear leveling, then introducing a logical block-device
boundary for FatFs without changing existing SD behavior. No empty FTL or
block-device scaffolding is included in this raw-driver milestone.
