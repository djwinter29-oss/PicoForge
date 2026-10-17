# Physical Storage Transports

This folder owns the reusable physical storage engines for RP2040 and RP2350. This document is the reference for
transport ownership, lifecycle, shared-pin wiring, and lab hardware records. Device protocols and filesystem policy
remain above this layer:

- [SD-card design](../../../../docs/sdcard-design.md): initialization, commands, registers, CRC validation,
  addressing, and sector/stream policy.
- [NOR flash](../../../../docs/nor-flash.md): identification, quad enable, programming, erase, and standalone
  LevelX integration.
- [CDC CLI guide](../../../../docs/cdc-cli.md): filesystem commands and mounting policy.
- [Hardware setup](../../../../docs/hardware-setup.md): board selection, power, debug connections, and other GPIOs.

## Physical Storage Bus

```text
drivers/
  storage/
    storage_transport.h / storage_transport.c
    spi/storage_spi.h / storage_spi.c
    sdio_pio/storage_sdio_pio.h / storage_sdio_pio.c / storage_sdio_pio.pio
    qspi_pio/storage_qspi_pio.h / storage_qspi_pio.c / storage_qspi_pio.pio
  sd_card/       SD initialization, registers, framing, sector and stream policy
  nor_flash/     NOR opcodes, status, page programming, erase and verification
```

The [common bus interface](storage_transport.h) dispatches initialization, clock changes with calculated actual rates,
hardware abort, deinitialization, and CS-bounded serial transactions. Its `pf_storage_transport_t` is a non-owning handle:
the caller owns the concrete context and serializes access. It creates no task, queue, filesystem policy, or automatic
mode-switch manager.

| Physical mode | Implementation | Device configurations |
| --- | --- | --- |
| SPI | [storage_spi.h](spi/storage_spi.h), [storage_spi.c](spi/storage_spi.c) | SD-SPI and NOR-SPI |
| Native SD PIO | [storage_sdio_pio.h](sdio_pio/storage_sdio_pio.h), [storage_sdio_pio.c](sdio_pio/storage_sdio_pio.c), [PIO programs](sdio_pio/storage_sdio_pio.pio) | Native four-bit SD memory cards |
| QSPI-PIO | [storage_qspi_pio.h](qspi_pio/storage_qspi_pio.h), [storage_qspi_pio.c](qspi_pio/storage_qspi_pio.c), [PIO programs](qspi_pio/storage_qspi_pio.pio) | NOR single/quad-lane serial transfers |

SPI and QSPI use command/address/dummy/data transactions. SD-SPI additionally uses the SPI byte-exchange and CS
primitives so command responses, data tokens, busy polling, and multiblock transfers retain their existing CS
boundaries. Native SD uses a distinct typed wire interface for command/response and data phases; it is not emulated
as a serial flash transaction. Native DMA buffers remain batchable so the SD layer can overlap CRC checking and
payload copying with receive DMA. Here, SDIO means native SD memory-card signaling, not support for SDIO I/O cards.

The transport owns GPIO mux/direction, programmed clock timing, PIO/DMA resources, FIFO/IRQ state, hardware waits,
and hardware cleanup. Device adapters retain SD commands, addressing, register decoding and stream state, or NOR
opcodes and erase/program behavior. They translate physical errors into their existing public device results.
SPI supports bounded FIFO polling for NOR; SD-SPI retains SDK blocking bulk I/O with protocol-level deadlines.

### Lifecycle and Shared Pins

Configure and zero-initialize the concrete context, obtain its non-owning bus handle, then initialize it before
device-protocol operations. Contexts must outlive all synchronous calls; do not copy a live context. The common
interface exposes `pf_storage_transport_init`, `pf_storage_transport_set_clock`, `pf_storage_transport_transfer`,
`pf_storage_transport_abort`, and `pf_storage_transport_deinit`. Native command/data operations use the native typed API rather
than `pf_storage_transport_transfer`.

[Board support](../../board/) remains authoritative for GP2-GP8. SD-SPI and NOR-SPI use one shared SPI implementation;
native SD and quad-SPI use their own physical engines. These are alternative modes of the shared pins, not permission
to attach multiple active devices. DET remains a board/device-presence input.

The standalone serial-storage profile is defined in [board_profile.c](../../board/board_profile.c) and exposed through
[board_storage.h](../../board/board_storage.h). [board_storage.c](../../board/board_storage.c) owns the mutable
serial runtime context, separate from the immutable profile. Board preparation supplies a physical descriptor and applies SPI
WP/HOLD idle levels; it does not initialize the bus or depend on NOR. The NOR adapter initializes that descriptor.
After device operations, the caller deinitializes the bus before releasing board-specific pins.

### Pin Allocation and Runtime State

[storage_transport.h](storage_transport.h) defines SDK-independent `pf_storage_spi_pins_t`, `pf_storage_sdio_pins_t`, and
`pf_storage_qspi_pins_t`. Native DAT0-DAT3 and quad IO0-IO3 are explicit arrays in logical order; the physical engines
validate their ascending, consecutive mapping. Pin allocation types do not include card detect, WP/HOLD policy,
controller pointers, clock rates, or mutable hardware ownership.

The immutable board resource profile contains `native_pins`, `spi_pins`, and `qspi_pins`, derived from one connector
signal assignment in [board_profile.c](../../board/board_profile.c). SD and NOR share `spi_pins`; neither has a second
board-specific SPI map. DET polarity/pulls and SPI WP/HOLD remain board/device configuration.

SD configuration and concrete storage contexts embed the corresponding shared type as `pins`. Board integration copies
that allocation into the selected context; mutable clock settings, controller references, PIO state machines, DMA
channels, and initialization flags remain runtime state, never fields modified in the board profile. Re-preparing
serial storage after deinitialization restores its configuration from that profile.

Direct configuration callers must migrate former flat fields to `pins`, for example `config.pins.clock_gpio` and
`config.pins.cs_gpio` (formerly SD-SPI `chip_select_gpio`). QSPI callers now supply all four `pins.data_gpio` entries
rather than only a base GPIO. Transport selection, board wiring, and transfer behavior are unchanged.
For SD API naming migration, see the [SD architecture](../../../../docs/sdcard-design.md#architecture).

Before switching devices, the caller must sync/unmount as appropriate, finish the device protocol, deinitialize the
bus, and satisfy the new device's power requirements. Bus abort/deinit never claims to finish an interrupted write
or make filesystem data durable.

## Memory and Storage Connections

Use GP2-GP7 for one memory device and choose one mode column below. GP7 is the CS pin in SD-SPI, QSPI-PIO, and SPI-NOR
modes; it is controlled by GPIO/PIO, not automatic hardware CS. SDIO has no CS signal.

| MCU GPIO | Header pin | SDIO-PIO | SD-SPI (SPI0) | QSPI-PIO | SPI-NOR flash (SPI0) |
| --- | --- | --- | --- | --- | --- |
| GP2 | 4 | CLK | CLK / SCK | CLK | CLK / SCK |
| GP3 | 5 | CMD | SI / MOSI | IO0 | IO0 / MOSI |
| GP4 | 6 | DAT0 | SO / MISO | IO1 | IO1 / MISO |
| GP5 | 7 | DAT1 | - | IO2 | WP (hold high) |
| GP6 | 9 | DAT2 | - | IO3 | HOLD (hold high) |
| GP7 | 10 | DAT3 | CS (GPIO-controlled) | CS | CS (GPIO-controlled) |
| GP8 | 11 | DET (optional) | DET (optional) | - | - |

- `-` means unused in that mode, not necessarily disconnected. GP8 is optional SD-card detect.
- Use 3.3 V and common ground, never 5 V. Power off before replacing a breakout.
- SDIO-PIO requires logical DAT0-DAT3 on four ascending consecutive GPIOs, in that order; lane swaps are unsupported.
  The board profile assigns CLK=GP2, CMD=GP3, and DAT0-DAT3 to GP4/GP5/GP6/GP7. PIO samples the contiguous GP4-GP7
  window directly.
- The SD-SPI driver holds GPIO-controlled CS low across each complete card transaction, including data and
  busy-response phases. Verify CS timing, clock, and data with a logic analyzer during board validation.
- SDIO has no CS signal. In SD-SPI mode, the driver controls the breakout's DAT3/CS line on GP7 as ordinary GPIO;
  no automatic hardware CS is used.
- Pin names follow the breakout guides below. Historical Pico 2 SDIO and SPI filesystem CLI operations are recorded
  below; they do not qualify current firmware, other boards/interface modes, or signal integrity.

## Lab Hardware Examples

The lab uses two types of memory/storage breakout: the Adafruit microSD board and W25Q128 flash board described below.
These are reference examples, not an exclusive list of compatible devices. Other SD-card or SPI/QSPI memory devices
can use this pin allocation if their supply and logic levels, signal functions, pull-ups, and timing requirements are
compatible. Check each device's pinout and protocol; firmware may need device-specific initialization or commands.
The active-high DET behavior and WP/HOLD requirements below describe these particular lab boards and must be checked
for any replacement. Use in the lab does not establish that all interface modes have been validated.

### SD-Card Breakout

The selected module is Adafruit's
[Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its
[pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Wire
DAT0/DAT1/DAT2/DAT3-CS to GP4/GP5/GP6/GP7 respectively in both modes. DAT1 and DAT2 are unused inputs in SD-SPI mode;
GP7 drives CS in that mode. Board support can construct either transport configuration and sets its active-high DET
signal on GP8 using the breakout's pull-up, without an MCU pull. Both drivers explicitly disable inherited MCU pulls
when the external DET pull-up is selected. The breakout and driver both provide SDIO data pull-ups, so check their
combined strength. The storage worker initializes and mounts detected cards according to
[the mounting policy](../../../../docs/cdc-cli.md). SPI requests use single-block commands for each sector.
Mode changes require protocol reinitialization. When switching from SPI to native SDIO, fully remove card power:
an MCU reset alone can leave the card latched in SPI mode. Fixed wiring does not imply hot switching.

#### Historical SD-Card Qualification

These records describe the firmware, cards, clocks, and wiring tested at the time. They are not qualification of the
current transport extraction or a throughput guarantee. See the
[SD-card validation limits](../../../../docs/sdcard-design.md#validation-and-limits) for current test scope.

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

#### Class 4 Read-Only Bring-Up (2026-10-15)

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

### W25Q128 Flash Breakout

The [Adafruit QSPI DIP Breakout - W25Q128](https://www.adafruit.com/product/5634) is a 3.3 V, 16 MB serial NOR flash
supporting QSPI and SPI; see its
[pinout guide](https://learn.adafruit.com/adafruit-qspi-dip-breakout-boards/pinouts). Connect CLK, IO0, IO1, IO2, IO3, and
CS to GP2, GP3, GP4, GP5, GP6, and GP7 respectively in both modes. This keeps IO0-IO3 on consecutive GP3-GP6 and shares
CS on GP7 with the SD breakout. Use pull-ups on GP7/CS, GP5/IO2/WP, and GP6/IO3/HOLD to keep them high through reset;
in SPI-NOR mode, keep GP5/GP6 high or as inputs with those external pull-ups. Never hard-tie IO2/IO3 to 3V3 when they
are used as QSPI data signals. The QSPI-PIO engine samples the contiguous data window GP3-GP6; the NOR protocol
configures the flash's quad-enable setting before quad reads. Mode changes require protocol reinitialization, not
just changing the GPIO mux. See [external NOR support](../../../../docs/nor-flash.md) for the standalone raw-driver
build, supported command set and validation limits. NOR hardware operation and FatFs support are not yet qualified
or implemented, respectively.

## Tests and Qualification

Host C tests cover common lifecycle dispatch and concrete transports under
[firmware/tests/](../../../tests/drivers/). The assembled
[SDIO](../../../../tests/pio/test_sdio_pio_timing.py) and
[QSPI](../../../../tests/pio/test_nor_qspi_timing.py) instruction models check wire timing assumptions;
[storage-layer contracts](../../../../tests/contracts/test_storage_layers.py) check the directory structure and
physical-driver header independence. Use the existing [test guidance](../../../../tests/README.md) for commands.
Host tests, firmware builds, and PIO models do not establish electrical margins, card compatibility, sustained
throughput, or power-loss safety.
