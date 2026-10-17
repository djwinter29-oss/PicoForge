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
    bus.h / bus.c
    serial.h / serial.c
    sd_wire.h / sd_wire.c
    spi/spi.h / spi.c
    sdio_pio/sdio_pio.h / sdio_pio.c / sdio_pio.pio
    sdio_pio/ports/freertos/sdio_wait.c
    qspi_pio/qspi_pio.h / qspi_pio.c / qspi_pio.pio
  sd_card/       SD initialization, registers, framing, sector and stream policy
  nor_flash/     NOR opcodes, status, page programming, erase and verification
```

The [common bus interface](bus.h) contains shared pin types, lifecycle, clock and platform services.
Wire operations are separate SDK-independent capabilities: [raw serial](serial.h) and [SD CMD/DAT](sd_wire.h).
`pf_storage_bus_t` is a non-owning descriptor: board/storage integration owns the concrete context, and the caller
serializes access. Protocol clients
do not dereference backend contexts or operations, include concrete backend/SDK headers, or own GPIO/PIO/DMA state.
It creates no task, queue, filesystem policy, or automatic mode-switch manager.
Common mode tags describe SPI, native SD, or QSPI signaling, not the backend's PIO/SPI-controller implementation.
Backend filenames, functions, types, and header guards use the same module name:

| Module | C API and types | Header guard |
| --- | --- | --- |
| SPI | `pf_spi_*`, `pf_spi_t` | `PF_SPI_H` |
| Native SD PIO | `pf_sdio_pio_*`, `pf_sdio_pio_t` | `PF_SDIO_PIO_H` |
| QSPI PIO | `pf_qspi_pio_*`, `pf_qspi_pio_t` | `PF_QSPI_PIO_H` |

The native wait-hook type is `pf_sdio_pio_wait_hook_t`; generated PIO programs use `pf_sdio_pio_*` or
`pf_qspi_pio_*`. These replace the former backend `pf_storage_*` names without compatibility aliases.
The common and wire interfaces use `pf_storage_*` / `PF_STORAGE_*` symbols. Concrete clock ownership and PIO/DMA
resource state remain private to each backend.

| Interface | C namespace | Header guard |
| --- | --- | --- |
| Common bus | `pf_storage_bus_*`, `pf_storage_bus_t` | `PF_STORAGE_BUS_H` |
| Raw serial | `pf_storage_serial_*` | `PF_STORAGE_SERIAL_H` |
| SD CMD/DAT wire | `pf_storage_sd_wire_*` | `PF_STORAGE_SD_WIRE_H` |

Backend factories are `pf_spi_bus`, `pf_sdio_pio_bus`, and `pf_qspi_pio_bus`. The common operations expose
`serial` and `sd_wire` capabilities. The bus result/mode constants use `PF_STORAGE_BUS_*`; SD wire diagnostics use
`PF_STORAGE_SD_WIRE_*`. Callers must use these names; no compatibility aliases are provided.
Device configuration members named `transport` and NOR's protocol transaction API remain unchanged.

| Physical mode | Implementation | Device configurations |
| --- | --- | --- |
| SPI | [spi.h](spi/spi.h), [spi.c](spi/spi.c) | SD-SPI and NOR-SPI |
| Native SD PIO | [sdio_pio.h](sdio_pio/sdio_pio.h), [sdio_pio.c](sdio_pio/sdio_pio.c), [PIO programs](sdio_pio/sdio_pio.pio) | Native four-bit SD memory cards |
| QSPI-PIO | [qspi_pio.h](qspi_pio/qspi_pio.h), [qspi_pio.c](qspi_pio/qspi_pio.c), [PIO programs](qspi_pio/qspi_pio.pio) | NOR single/quad-lane serial transfers |

SPI and QSPI execute ordered raw phases under one CS assertion. Each phase specifies borrowed TX/RX buffers, bit count,
and one or four lanes; an unbuffered phase drives ones. All phases are validated before CS is asserted. Buffered phases
must contain whole bytes; hardware SPI also requires whole-byte clock-only phases, while QSPI permits individual clocks.
Quad phases are half-duplex. The NOR adapter builds opcode/address/dummy/payload phases; physical engines do not parse
them. SD-SPI uses manual CS and byte exchange so response, token, busy-polling and multiblock boundaries stay unchanged.

Native CMD exchange takes caller-framed bytes and returns response bytes, including partial capture on failure.
The current PIO engine supports six TX bytes and byte-aligned responses up to 136 bits; it never builds CRC or interprets
card status. Separate DAT start/poll/progress/finish/cancel operations preserve RX-before-CMD ordering and DMA overlap.
They are not replaced by blocking read/write calls. Here, SDIO means native SD memory-card signaling, not support for
SDIO I/O cards.

The transport owns GPIO mux/direction, programmed clock timing, PIO/DMA resources, FIFO/IRQ state, hardware waits,
and hardware cleanup. Device adapters retain SD commands, addressing, register decoding and stream state, or NOR
opcodes and erase/program behavior. They translate physical errors into their existing public device results.
SPI supports bounded FIFO polling for NOR; SD-SPI retains SDK blocking bulk I/O with protocol-level deadlines.

### Lifecycle and Shared Pins

Configure and zero-initialize the concrete context, obtain its non-owning bus handle, then initialize it before
device-protocol operations. Contexts must outlive all synchronous calls; do not copy a live context. The common
interface exposes `pf_storage_bus_init`, `pf_storage_bus_set_clock`, `pf_storage_bus_abort`, and
`pf_storage_bus_deinit`. Wire dispatch uses `pf_storage_serial_*` or `pf_storage_sd_wire_*` from their capability
headers; the common lifecycle has no device-shaped transfer operation. Concrete backend headers are only for
board/storage composition and physical-engine tests.

Native clients observe only availability/health, actual/maximum clock and the raw DAT0 level. Physical ownership and
PIO response packing are not exposed. Legacy packed-response diagnostics are retained by the SD host, not the transport.
Error/idle callbacks are borrowed and invoked synchronously. TX start acquires physical TX programs internally and retains
them across adjacent payloads. Returning to CMD signaling is a hardware-only idle operation; SD recovery still decides
when CMD12 is required and never replays an uncertain write. Wire-capability validation rejects incomplete operation sets
before device initialization.

Released-line clock generation takes a count at the current rate and a caller deadline. The SD host supplies the
400 kHz initialization rate, 80 clocks, command deadlines and DAT0 wait policy; clock generation does not select them.

Platform time, bounded delays, and optional scheduler-wait setup are also dispatched through the descriptor.
The [FreeRTOS wait adapter](sdio_pio/ports/freertos/sdio_wait.c) owns PIO IRQ details; SD's execution port only
requests that the transport enable it. The backend retains its physical configuration and installed wait hook across
common-interface initialization failures and deinitialization, so reinitialization does not need device-side pin setup.

[Board support](../../board/) remains authoritative for GP2-GP8. SD-SPI and NOR-SPI use one shared SPI implementation;
native SD and quad-SPI use their own physical engines. These are alternative modes of the shared pins, not permission
to attach multiple active devices. DET remains a board/device-presence input.

The standalone serial-storage profile is defined in [board_profile.c](../../board/board_profile.c) and exposed through
[board_storage.h](../../board/board_storage.h). [board_storage.c](../../board/board_storage.c) owns the mutable
serial runtime context, separate from the immutable profile. Board preparation supplies a physical descriptor and applies SPI
WP/HOLD idle levels; it does not initialize the bus or depend on NOR. The NOR adapter initializes that descriptor.
After device operations, the caller deinitializes the bus before releasing board-specific pins.
For SD, [board.c](../../board/board.c) owns the selected static backend context and prepares card-detect GPIO/pulls
from the same immutable profile. Its descriptor carries an optional board presence callback; without one, presence
is unknown and media is assumed present. Repeated configuration queries do not reset a live backend.

### Pin Allocation and Runtime State

[bus.h](bus.h) defines SDK-independent `pf_storage_spi_pins_t`, `pf_storage_sdio_pins_t`, and
`pf_storage_qspi_pins_t`. Native DAT0-DAT3 and quad IO0-IO3 are explicit arrays in logical order; the physical engines
validate their ascending, consecutive mapping. Pin allocation types do not include card detect, WP/HOLD policy,
controller pointers, clock rates, or mutable hardware ownership.

The immutable board resource profile contains `native_pins`, `spi_pins`, and `qspi_pins`, derived from one connector
signal assignment in [board_profile.c](../../board/board_profile.c). SD and NOR share `spi_pins`; neither has a second
board-specific SPI map. DET polarity/pulls and SPI WP/HOLD remain board/device configuration.

Concrete storage contexts embed the corresponding shared type as `pins`. Board integration copies that allocation
into the selected context; mutable clock settings, controller references, PIO state machines, DMA
channels, and initialization flags remain runtime state, never fields modified in the board profile. Re-preparing
serial storage after deinitialization restores its configuration from that profile.

SD configuration callers supply a persistent descriptor in `pf_sd_config_t.transport`, plus the protocol's
transfer-clock target in `clock_hz`; its mode replaces the former separate SD transport tag and backend union.
Pin allocation, controller selection, and DET configuration are no longer SD
configuration fields. Backend integration supplies `pins.clock_gpio`, `pins.cs_gpio`, or all four `pins.data_gpio`
entries as appropriate. The former `storage_bus.[ch]`, `pf_storage_bus_*`, and `PF_STORAGE_BUS_*` names are replaced
by `bus.[ch]`, `pf_storage_bus_*`, and `PF_STORAGE_BUS_*`, without compatibility aliases.
Physical backend descriptor factories now end in `_transport`. Transport selection, board wiring, and transfer
behavior are unchanged.
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
