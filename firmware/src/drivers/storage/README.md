# Physical Storage Transports

This folder owns the reusable physical storage engines for RP2040 and RP2350. This document is the reference for
transport ownership, lifecycle, shared-pin wiring, and lab hardware records. Device protocols and filesystem policy
remain above this layer:

- [SD-card design](../../../../docs/sdcard-design.md): initialization, commands, registers, CRC validation,
  addressing, and sector/stream policy.
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
    sdio_pio/sdio_wait_freertos.c
  sd_card/       SD initialization, registers, framing, sector and stream policy
```

The [common bus interface](bus.h) contains shared pin types, lifecycle, clock and platform services.
Wire operations are separate SDK-independent capabilities: [raw serial](serial.h) and [SD CMD/DAT](sd_wire.h).
`pf_storage_bus_t` is a non-owning descriptor: board/storage integration owns the concrete context, and the caller
serializes access. Protocol clients
do not dereference backend contexts or operations, include concrete backend/SDK headers, or own GPIO/PIO/DMA state.
It creates no task, queue, filesystem policy, or automatic mode-switch manager.
Common mode tags describe SPI or native SD signaling, not the backend's PIO/SPI-controller implementation.
Backend filenames, functions, types, and header guards use the same module name:

| Module | C API and types | Header guard |
| --- | --- | --- |
| SPI | `pf_spi_*`, `pf_spi_t` | `PF_SPI_H` |
| Native SD PIO | `pf_sdio_pio_*`, `pf_sdio_pio_t` | `PF_SDIO_PIO_H` |

The native wait-hook type is `pf_sdio_pio_wait_hook_t`; generated PIO programs use `pf_sdio_pio_*`.
The common and wire interfaces use `pf_storage_*` / `PF_STORAGE_*` symbols. Concrete clock ownership and PIO/DMA
resource state remain private to each backend.

| Interface | C namespace | Header guard |
| --- | --- | --- |
| Common bus | `pf_storage_bus_*`, `pf_storage_bus_t` | `PF_STORAGE_BUS_H` |
| Raw serial | `pf_storage_serial_*` | `PF_STORAGE_SERIAL_H` |
| SD CMD/DAT wire | `pf_storage_sd_wire_*` | `PF_STORAGE_SD_WIRE_H` |

Backend factories are `pf_spi_bus` and `pf_sdio_pio_bus`. The common operations expose
`serial` and `sd_wire` capabilities. The bus result/mode constants use `PF_STORAGE_BUS_*`; SD wire diagnostics use
`PF_STORAGE_SD_WIRE_*`. Callers must use these names; no compatibility aliases are provided.
Device configuration members named `transport` remain unchanged.

| Physical mode | Implementation | Device configurations |
| --- | --- | --- |
| SPI | [spi.h](spi/spi.h), [spi.c](spi/spi.c) | SD-SPI |
| Native SD PIO | [sdio_pio.h](sdio_pio/sdio_pio.h), [sdio_pio.c](sdio_pio/sdio_pio.c), [PIO programs](sdio_pio/sdio_pio.pio) | Native four-bit SD memory cards |

SD-SPI uses manual CS and byte exchange so response, token, busy-polling and multiblock boundaries stay unchanged.

Native CMD exchange takes caller-framed bytes and returns response bytes, including partial capture on failure.
The current PIO engine supports six TX bytes and byte-aligned responses up to 136 bits; it never builds CRC or interprets
card status. Separate DAT start/poll/progress/finish/cancel operations preserve RX-before-CMD ordering and DMA overlap.
They are not replaced by blocking read/write calls. Here, SDIO means native SD memory-card signaling, not support for
SDIO I/O cards.

The transport owns GPIO mux/direction, programmed clock timing, PIO/DMA resources, FIFO/IRQ state, hardware waits,
and hardware cleanup. The SD adapter retains commands, addressing, register decoding and stream state, and translates
physical errors into its existing public device results.
SD-SPI retains SDK blocking bulk I/O with protocol-level deadlines.

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

Both native TX programs park on their completion IRQ until the CPU disables the data state machine. They do not
wrap into an empty FIFO pull after a completed payload, which would set TXSTALL and falsely report an underrun.
The next TX start clears the old completion flag while the state machine is disabled; real FIFO stalls remain fatal.

Default-Speed RX qualifies start and samples payload after rising CLK, half a bit after the card's falling-edge
launch. Sampling just after falling CLK could miss the start bit or capture changing data; extending only the payload
delay did not fix start detection. High-Speed RX samples after falling CLK instead, following the card's rising-edge
launch. Both receive programs run at the system clock, not a fractional data divider, so WAIT-to-IN latency is one
system cycle. TX retains its separately synchronized clock/data dividers. Timing-model checks cover cadence and
framing; actual card/pad timing still requires hardware qualification.

#### Clock Planning

`pf_sdio_pio_set_clock` plans every request in one place and never exceeds it. The command/clock state machine spends
two system cycles per bus period, so its divider cannot be below 2.0 and the fastest bus is SYSCLK / 4.

| Request | Divider | Consequence |
| --- | --- | --- |
| Up to 25 MHz (Default Speed) | Whole cycles, rounded up | Symmetric CLK at any SYSCLK; 150 MHz gives exactly 25 MHz, 125 MHz gives 20.83 MHz |
| Above 25 MHz (High Speed) | Even, in 1/256 cycles, from 2.0 to 3.0 | Clamps to SYSCLK / 4 when SYSCLK is too low (150 MHz gives 37.5 MHz) |
| Above 50 MHz, or a High-Speed divider above 3.0 | Rejected | The bus is left unchanged and a STATE error is reported |

* The cycle-locked High-Speed TX data state machine runs at exactly half the clock divider. The divider is therefore
  halved before rounding and doubled; an odd value would slip one system cycle every 128 bus periods, which is eight
  cycles across one 1,040-nibble block.
* The shortened High-Speed RX loop rearms three system cycles after it sees CLK fall. A CLK low phase of four or more
  cycles would satisfy the next `WAIT 0` early and count one bit period as two nibbles. Because fractional dividers
  alternate whole-cycle phases and their placement is not documented, the planner accepts only dividers up to 3.0,
  where both floor and ceiling placement keep every low phase at three cycles or less. The timing-model tests check
  both placements.
* Receive and transmit program selection follows the *request* (above 25 MHz selects High-Speed card timing), not the
  rounded rate, so a clamped High-Speed request keeps High-Speed sampling.
* The reported maximum clock is what the planner accepts for a 50 MHz request (50 MHz where SYSCLK / 4 allows it),
  otherwise 25 MHz. The SD host attempts High Speed only when this exceeds 25 MHz.

Host tests sweep SYSCLK from 100 to 300 MHz for divider parity, rounding and limits. Hardware has qualified only
150 MHz / 25 MHz and 200 MHz / 50 MHz; other High-Speed combinations are modeled, not proven on a card.

Released-line clock generation takes a count at the current rate and a caller deadline. The SD host supplies the
400 kHz initialization rate, 80 clocks, command deadlines and DAT0 wait policy; clock generation does not select them.

Platform time, bounded delays, and optional scheduler-wait setup are also dispatched through the descriptor.
The [FreeRTOS wait adapter](sdio_pio/sdio_wait_freertos.c) owns PIO IRQ details; SD's execution port only
requests that the transport enable it. The backend retains its physical configuration and installed wait hook across
common-interface initialization failures and deinitialization, so reinitialization does not need device-side pin setup.

[Board support](../../board/) remains authoritative for GP2-GP8. SD-SPI and native SD use their respective physical
engines. These are alternative modes of the shared pins, not permission to attach multiple active devices. DET remains
a board/card-presence input.

[Board support](../../board/board_profile.c) defines the immutable SD pin profile.
[board.c](../../board/board.c) owns the selected static SD transport context and prepares card-detect GPIO/pulls
from that profile. Its descriptor carries an optional board presence callback; without one, presence is unknown and
media is assumed present. Repeated configuration queries do not reset a live transport.

### Pin Allocation and Runtime State

[bus.h](bus.h) defines SDK-independent `pf_storage_spi_pins_t` and `pf_storage_sdio_pins_t`. Native DAT0-DAT3 are
an explicit array in logical order; the physical engine validates their ascending, consecutive mapping. Pin allocation
types do not include card detect, controller pointers, clock rates, or mutable hardware ownership.

The immutable board resource profile contains `native_pins` and `spi_pins`, derived from one connector signal
assignment in [board_profile.c](../../board/board_profile.c). DET polarity and pulls remain board configuration.

Concrete storage contexts embed the corresponding shared type as `pins`. Board integration copies that allocation
into the selected context; mutable clock settings, controller references, PIO state machines, DMA
channels, and initialization flags remain runtime state, never fields modified in the board profile.

SD configuration callers supply a persistent descriptor in `pf_sd_config_t.transport`, plus the protocol's
transfer-clock target in `clock_hz`. Pin allocation, controller selection, and DET configuration belong to board
integration, not the SD protocol configuration. Backend integration supplies `pins.clock_gpio`, `pins.cs_gpio`, or
all four `pins.data_gpio` entries as appropriate.
Physical backend descriptor factories are `pf_spi_bus` and `pf_sdio_pio_bus`.
See the [SD architecture](../../../../docs/sdcard-design.md#architecture) for the protocol configuration contract.

Before switching SD transports, the caller must sync/unmount as appropriate, finish the card protocol, deinitialize
the bus, and satisfy the card's power requirements. Bus abort/deinit never claims to finish an interrupted write or
make filesystem data durable.

## Memory and Storage Connections

Use GP2-GP7 for one SD card and choose one mode below. GP7 is the CS pin in SD-SPI and is controlled by GPIO, not
automatic hardware CS. SDIO has no CS signal.

| MCU GPIO | Header pin | SDIO-PIO | SD-SPI (SPI0) |
| --- | --- | --- | --- |
| GP2 | 4 | CLK | CLK / SCK |
| GP3 | 5 | CMD | SI / MOSI |
| GP4 | 6 | DAT0 | SO / MISO |
| GP5 | 7 | DAT1 | - |
| GP6 | 9 | DAT2 | - |
| GP7 | 10 | DAT3 | CS (GPIO-controlled) |
| GP8 | 11 | DET (optional) | DET (optional) |

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

The lab uses the Adafruit microSD breakout described below as a reference example, not an exclusive list of compatible
devices. Check each card and breakout's pinout, supply, logic levels, pull-ups and timing requirements. Use in the lab
does not establish that all cards or interface modes have been validated.

### SD-Card Breakout

The selected module is Adafruit's
[Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its
[pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Wire
DAT0/DAT1/DAT2/DAT3-CS to GP4/GP5/GP6/GP7 respectively in both modes. DAT1 and DAT2 are unused inputs in SD-SPI mode;
GP7 drives CS in that mode. Board support can construct either transport configuration and sets its active-high DET
signal on GP8 using the breakout's pull-up, without an MCU pull. Both drivers explicitly disable inherited MCU pulls
when the external DET pull-up is selected. The breakout and driver both provide SDIO data pull-ups, so check their
combined strength. The storage worker initializes and mounts detected cards according to
[the mounting policy](../../../../docs/cdc-cli.md). SPI uses CMD18/CMD25 for multi-sector requests and CMD17/CMD24
for single sectors. Card capability negotiation and the board's 50 MHz SPI ceiling are defined by the
[SPI transfer-clock policy](../../../../docs/sdcard-design.md#spi-transfer-clock); this is not a guaranteed 50 MHz bus.
Mode changes require protocol reinitialization. When switching from SPI to native SDIO, fully remove card power:
an MCU reset alone can leave the card latched in SPI mode. Fixed wiring does not imply hot switching.

#### SPI Clock Qualification (2026-10-17)

The connected SanDisk `SB128` (128 GB, product revision 8.0) returned a CRC-valid CMD6 check status with function-group-1
support mask `0x8001`: Default Speed is supported, but the High-Speed function-1 bit is clear. The driver therefore
retained 25 MHz even with the board's 50 MHz request. This establishes this card's advertised SPI limit, not the limits
of its native SD mode or every card with the same product name. The initial switch from native firmware failed at
CMD58; card removal/reinsertion followed by a firmware reset restored SPI initialization. The final SPI argument
also clears ACMD41's reserved voltage-window bits; that change alone was not isolated as the cause of recovery.

Clean version `0.1.0` firmware passed the filesystem write/read self-test on the existing exFAT volume:

| Pico 2 SYSCLK / actual SPI SCK | Test size | Write / read | Result |
| --- | --- | --- | --- |
| 150 MHz / 25 MHz | 128 MiB | 1.91 / 1.98 MB/s | Passed; zero mismatches and errors; test file removed |
| 200 MHz / 25 MHz | 32 MiB | 2.02 / 2.10 MB/s | Passed; zero mismatches and errors; test file removed |

Only `/PFTEST.BIN` was used, without formatting or replacing other files. The board was left at stock 150 MHz with
25 MHz SPI, the filesystem mounted, and MSC disabled. CMD6 check/switch sequencing, CRC/error rejection, rate rounding,
and unsupported-card limits have host regression tests. A card advertising SPI High Speed is still required to
qualify physical 37.5/41.67/50 MHz operation; no 50 MHz SPI hardware pass is claimed here.

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

#### TX Completion Regression (2026-10-17)

The replacement 8 GB `SD8GB` card initialized and mounted exFAT at 25 MHz, but writes failed with TXSTALL. Both TX
programs signaled completion and wrapped into an empty blocking PULL before the CPU disabled the state machine.
Parking on `IRQ WAIT 2` removes that false underrun without weakening actual starvation detection or changing wire timing.

The fixed image was flashed and verified on Pico 2 at 150 MHz CPU / 25 MHz SD. An authorized 1 MiB file write/read
self-test passed with zero mismatches and cleanup (2.55 MB/s write, 7.44 MB/s read). The dashboard subsequently
reported a passed 32 MiB test with zero mismatches and cleanup (7.19 MB/s write, 7.48 MB/s read). Final card diagnostics
had no CRC, timeout, or FIFO-stall errors, and `/PFTEST.BIN` was absent. These checks do not qualify other cards,
electrical timing margins, optional 50 MHz operation, endurance, or power-loss safety.

#### High-Speed Bring-up (2026-10-17)

The same card exposed a one-bit-shifted SCR at 200 MHz CPU / 400 kHz SD before the Default-Speed RX edge correction
above. A fixed payload-only settling delay was insufficient. High-Speed qualification also failed without additional
settling after CMD6 selected the new function. The SD policy now leaves CLK running for 1 ms at the old rate after
DAT0 becomes ready, before raising the clock. This is a bounded lab-qualified delay, not a guarantee for every card.

At 200 MHz CPU / 50 MHz SD, three authorized 32 MiB write/read tests passed, including two intervening normal resets.
Each wrote and read all 33,554,432 bytes with zero mismatches and removed `/PFTEST.BIN`. Write throughput was
7.92-8.07 MB/s and read throughput 10.92 MB/s. GPIO drive/slew defaults were retained; no debugger qualification
breakpoints were used for these three tests. The user chose to leave this image running.

Before final inspection the card was changed to the 128 GB `SB128`. The dashboard separately reported a passed
128 MiB self-test with zero mismatches and cleanup at the same 200 MHz CPU / 50 MHz SD setting (15.47 MB/s write,
14.00 MB/s read). Final CDC inspection confirmed High-Speed timing, no CRC/timeout/stall errors, no fallback,
facade diagnostics `result=ok`, and `/PFTEST.BIN` absent. This was a separate dashboard test, not one of the three
controlled runs above.

The 250 MHz CPU / 50 MHz SD configuration still failed runtime I/O despite successful negotiation and occasional
valid sector captures. A captured CMD17 frame was correct at the MCU pins, but the card reported `COM_CRC_ERROR`;
this does not establish signal timing at the card socket. Neither fractional timing nor wiring is individually
proven to be the remaining cause. Do not treat a mounted 50 MHz volume, or a passing timing model, as sustained
qualification. CRC checks, the one-time startup fallback, and runtime invalidation remain enabled.

To reproduce the tested image without changing the default profile:

```sh
cmake -S firmware -B build/firmware-pico2-hs200 \
  -DPICO_BOARD=pico2 -DPF_SYS_CLK_MHZ=200 \
  -DPF_SD_CARD_PORT=FREERTOS -DPF_SD_CARD_TRANSPORT=SDIO_PIO \
  -DPF_SDIO_PIO_HIGH_SPEED=ON -DPF_SD_CARD_INTERRUPT_WAIT=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/firmware-pico2-hs200 --parallel 4
```

## Tests and Qualification

Host C tests cover common lifecycle dispatch and concrete transports under
[firmware/tests/](../../../tests/drivers/). The assembled
[SDIO](../../../../tests/pio/test_sdio_pio_timing.py) instruction model checks wire timing assumptions;
[storage-layer contracts](../../../../tests/contracts/test_storage_layers.py) check the directory structure and
physical-driver header independence. Use the existing [test guidance](../../../../tests/README.md) for commands.
Host tests, firmware builds, and PIO models do not establish electrical margins, card compatibility, sustained
throughput, or power-loss safety.
