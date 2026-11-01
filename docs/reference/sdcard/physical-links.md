# SD-Card Driver and Physical Links

The SD-card driver and its physical link engines for RP2040 and RP2350 live in
[`firmware/src/drivers/sdcard/`](../../../firmware/src/drivers/sdcard). This document is the
reference for physical transport ownership, lifecycle, and shared-pin constraints. Transport-specific protocol
sources live beside their physical engines; their card policy and filesystem integration are documented elsewhere:

- [SD-card design](driver.md): initialization, commands, registers, CRC validation,
  addressing, and sector/stream policy.
- [Storage lifecycle](../architecture.md#storage-ownership-lifecycle): mounting and ownership policy.
- [Hardware setup](../../guides/hardware.md): wiring, electrical requirements, and breakout.
- [Console guide](../../guides/console.md): filesystem commands.

## Physical Link

```text
drivers/sdcard/
  sd*.c / sd*.h          shared facade, contracts, board setup, protocol helpers and execution ports
  sd_link.h / .c        common declarations, descriptor validation and presence helpers
  spi/sd_link_spi.h     SPI CS and byte exchange declarations
  sdio/sd_link_sdio.h   SD CMD/DAT declarations
  spi/sd_backend_spi.c / sd_spi_internal.h / sd_board_spi.c
  spi/spi.h / spi.c / spi_pins.h
  sdio/sd_backend_sdio.c / sd_sdio_internal.h / sd_board_sdio.c
  sdio/sd_sdio_host.h / sd_sdio_host.c
  sdio/sdio_pio.h / sdio_pio.c / sdio_pio_pins.h / sdio_pio.pio
  sdio/sdio_pio_wait_freertos.c
```

The [common link interface](../../../firmware/src/drivers/sdcard/sd_link.h) contains descriptor/presence helpers,
physical lifecycle, clock and platform services. Pin types belong to the concrete engines. Wire operations are separate
SDK-independent interfaces:
[raw serial](../../../firmware/src/drivers/sdcard/spi/sd_link_spi.h) and
[SD CMD/DAT](../../../firmware/src/drivers/sdcard/sdio/sd_link_sdio.h).
`pf_sd_link_t` is a non-owning descriptor: board/storage integration owns the concrete context, and the caller
serializes access. Protocol clients
do not dereference backend contexts, include concrete backend/SDK headers, or own GPIO/PIO/DMA state.
It creates no task, queue, filesystem policy, or automatic mode-switch manager.
Common mode tags describe SPI or native SD signaling, not the backend's PIO/SPI-controller implementation.
Backend filenames, functions, types, and header guards use the same module name:

| Module | C API and types | Header guard |
| --- | --- | --- |
| SPI | `pf_spi_*`, `pf_spi_t` | `PF_SPI_H` |
| Native SD PIO | `pf_sdio_pio_*`, `pf_sdio_pio_t` | `PF_SDIO_PIO_H` |

The native wait-hook type is `pf_sdio_pio_wait_hook_t`; generated PIO programs use `pf_sdio_pio_*`.
The common and wire interfaces use `pf_sd_link_*` / `PF_SD_LINK_*` symbols. Concrete clock ownership and PIO/DMA
resource state remain private to each backend.

| Interface | C namespace | Header guard |
| --- | --- | --- |
| Common link | `pf_sd_link_*`, `pf_sd_link_t` | `PF_SD_LINK_H` |
| Raw serial | `pf_sd_link_spi_*` | `PF_SD_LINK_SPI_H` |
| SD CMD/DAT wire | `pf_sd_link_sdio_*` | `PF_SD_LINK_SDIO_H` |

Backend factories are `pf_spi_bus` and `pf_sdio_pio_bus`. CMake links exactly one backend, which directly implements
the common link functions and its own SPI or SD CMD/DAT functions. There are no operation tables, optional capabilities,
or runtime backend discovery. The descriptor retains context, signaling mode and optional board presence detection.
The bus result/mode constants use `PF_SD_LINK_*`; SD wire diagnostics use
`PF_SD_LINK_SDIO_*`. Callers must use these names; no compatibility aliases are provided.
Device configuration members named `transport` remain unchanged.

| Physical mode | Implementation | Device configurations |
| --- | --- | --- |
| SPI | [spi.h](../../../firmware/src/drivers/sdcard/spi/spi.h), [spi.c](../../../firmware/src/drivers/sdcard/spi/spi.c) | SD-SPI |
| Native SD PIO | [sdio_pio.h](../../../firmware/src/drivers/sdcard/sdio/sdio_pio.h), [sdio_pio.c](../../../firmware/src/drivers/sdcard/sdio/sdio_pio.c), [PIO programs](../../../firmware/src/drivers/sdcard/sdio/sdio_pio.pio) | Native four-bit SD memory cards |

SD-SPI uses manual CS and byte exchange so response, token, busy-polling and multiblock boundaries stay unchanged.

Native CMD exchange takes caller-framed bytes and returns response bytes, including partial capture on failure.
The current PIO engine supports six TX bytes and byte-aligned responses up to 136 bits; it never builds CRC or interprets
card status. Separate DAT start/poll/progress/finish/cancel operations preserve RX-before-CMD ordering and DMA overlap.
They are not replaced by blocking read/write calls. Here, SDIO means native SD memory-card signaling, not support for
SDIO I/O cards.

The transport owns GPIO mux/direction, programmed clock timing, PIO/DMA resources, FIFO/IRQ state, hardware waits,
and hardware cleanup. The SD adapter retains commands, addressing, register decoding and stream state, and translates
physical errors into its existing public device results.
The production SD-SPI binding leaves `pf_spi_t.timeout_us` at zero and uses SDK blocking bulk I/O.
Protocol polling deadlines do not interrupt a blocking exchange, so they are not hard end-to-end deadlines.
A nonzero engine timeout selects FIFO polling and limits consecutive intervals without progress, not total
transaction duration. This optional path does not change CS or replay bytes on failure.
The concrete SPI engine uses SD-link results and is not an independent general-purpose SPI service.

### Lifecycle and Shared Pins

Configure and zero-initialize the concrete context, obtain its non-owning bus handle, then initialize it before
device-protocol operations. Contexts must outlive all synchronous calls; do not copy a live context. The common
interface exposes `pf_sd_link_init`, `pf_sd_link_set_clock`, `pf_sd_link_abort`, and
`pf_sd_link_deinit`. Statically linked wire calls use `pf_sd_link_spi_*` or `pf_sd_link_sdio_*` from their interface
headers; the common lifecycle has no device-shaped transfer operation. Concrete backend headers are only for
board/storage composition and physical-engine tests.

Native clients observe only availability/health, actual/maximum clock and the raw DAT0 level. Physical ownership and
PIO response packing are not exposed. Legacy packed-response diagnostics are retained by the SD host, not the transport.
Error/idle callbacks are borrowed and invoked synchronously. TX start acquires physical TX programs internally and retains
them across adjacent payloads. Returning to CMD signaling is a hardware-only idle operation; SD recovery still decides
when CMD12 is required and never replays an uncertain write. Descriptor, buffer, deadline and hardware-state validation
remain; missing implementations are link errors rather than runtime capability failures.

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

Host tests sweep SYSCLK from 100 to 300 MHz for divider parity, rounding and limits. These checks model timing; they do not qualify hardware. Historical card measurements are listed in the
[test records](../../validation/records/README.md); release qualification remains configuration-specific.

Released-line clock generation takes a count at the current rate and a caller deadline. The SD host supplies the
400 kHz initialization rate, 80 clocks, command deadlines and DAT0 wait policy; clock generation does not select them.

Platform time, bounded delays, and optional scheduler-wait setup are statically linked backend functions.
The [FreeRTOS wait adapter](../../../firmware/src/drivers/sdcard/sdio/sdio_pio_wait_freertos.c) owns PIO IRQ details;
SD's execution port only
requests that the transport enable it. The backend retains its physical configuration and installed wait hook across
common-interface initialization failures and deinitialization, so reinitialization does not need device-side pin setup.

[Board support](../../../firmware/src/config) remains authoritative for GP2-GP8. SD-SPI and native SD use their
respective physical
engines. These are alternative modes of the shared pins, not permission to attach multiple active devices. DET remains
a board/card-presence input.

[Board support](../../../firmware/src/config) supplies the immutable pins and controller selection for both
transports. CMake selects [sd_board_sdio.c](../../../firmware/src/drivers/sdcard/sdio/sd_board_sdio.c) or
[sd_board_spi.c](../../../firmware/src/drivers/sdcard/spi/sd_board_spi.c), which owns the persistent transport context and
implements the same internal board-binding header. Shared
[sd_board.c](../../../firmware/src/drivers/sdcard/sd_board.c) applies the saved clock and prepares card-detect
GPIO/pulls.
Its descriptor carries an optional board presence callback; without one, presence is unknown and
media is assumed present. Repeated configuration queries do not reset a live transport.

### Pin Allocation and Runtime State

[sdio_pio_pins.h](../../../firmware/src/drivers/sdcard/sdio/sdio_pio_pins.h) and
[spi_pins.h](../../../firmware/src/drivers/sdcard/spi/spi_pins.h) define their backend-specific signal allocations;
[sdio_pio.h](../../../firmware/src/drivers/sdcard/sdio/sdio_pio.h) and
[spi.h](../../../firmware/src/drivers/sdcard/spi/spi.h) define the corresponding controller interfaces.
[sd_link.h](../../../firmware/src/drivers/sdcard/sd_link.h) contains only the common runtime link interface.
Native DAT0-DAT3 are an explicit array in logical
order, and the SDIO-PIO backend checks their ascending, consecutive mapping. The board profile describes both
transports, alongside shared UART, LED, and card-detect resources;
[sd_board.c](../../../firmware/src/drivers/sdcard/sd_board.c) uses the build-selected one. DET polarity
and pulls remain board configuration.

Board integration copies the selected allocation into its static transport context. Mutable clock settings,
controller references, PIO state machines, DMA channels, and initialization flags remain runtime state, never fields
modified in the board profile.

SD configuration callers supply a persistent descriptor in `pf_sd_config_t.transport`, plus the protocol's
transfer-clock target in `clock_hz`. Pin allocation, controller selection, and DET configuration belong to board
integration, not the SD protocol configuration. Backend integration supplies `pins.clock_gpio`, `pins.cs_gpio`, or
all four `pins.data_gpio` entries as appropriate.
Physical backend descriptor factories are `pf_spi_bus` and `pf_sdio_pio_bus`.
See the [SD architecture](driver.md#architecture) for the protocol configuration contract.

Before switching SD transports, the caller must sync/unmount as appropriate, finish the card protocol, deinitialize
the bus, and satisfy the card's power requirements. Bus abort/deinit never claims to finish an interrupted write or
make filesystem data durable.

## Wiring and Measurement Records

See [hardware setup](../../guides/hardware.md#memory-and-storage-connections) for the authoritative wiring,
electrical requirements, and breakout. Dated bring-up observations are preserved in
[bring-up records](../../validation/records/sdcard-bringup-observations.md), not current operating instructions.

## Tests and Qualification

Host C tests cover descriptor helpers and statically linked lifecycle/wire calls against concrete transports under
[firmware/tests/](../../../firmware/tests/drivers). The assembled
[SDIO](../../../tests/pio/test_sdio_pio_timing.py) instruction model checks wire timing assumptions;
[storage-layer contracts](../../../tests/contracts/test_storage_layers.py) check the directory structure and
physical-driver header independence. Use the existing [test guidance](../../../tests/README.md) for commands.
Host tests, firmware builds, and PIO models do not establish electrical margins, card compatibility, sustained
throughput, or power-loss safety.
