# SD-Card Driver Design

This document describes the implemented SD memory-card driver for RP2040 and
RP2350. It is the reference for driver interfaces, transport workflows, and
resource ownership. Wiring and electrical requirements remain in
[hardware setup](hardware-setup.md#memory-and-storage-connections); filesystem
commands and mounting policy remain in [the CDC CLI guide](cdc-cli.md).

## Architecture

One configured card is exposed through a transport-neutral, synchronous block
API. Native SDIO-PIO uses four data lanes; SD-SPI uses a Pico SDK SPI controller.
Here, SDIO means native SD memory-card signaling, not support for SDIO I/O cards.

```mermaid
flowchart TD
    Board[Board profile and build-time transport selection] --> Config[pf_board_get_sd_card_config]
    Config --> Card[SD-card configuration and dispatch]
    Storage[Storage middleware] --> FatFs[FatFs]
    FatFs --> Disk[FatFs disk-I/O adapter]
    Disk --> Gate[Block API and firmware ownership gate]
    MSC[USB MSC callbacks] --> Gate
    Gate --> Card
    Card --> Native[SDIO-PIO card lifecycle]
    Card --> SPI[SPI card lifecycle]
    Native --> Host[PIO host: commands, DMA, program switching]
    Native --> NativeProtocol[Native responses and four-lane CRC]
    SPI --> SPIProtocol[SPI response and data tokens]
    NativeProtocol --> Shared[Shared SD protocol helpers]
    SPIProtocol --> Shared
    SPI --> Shared
    Host --> PIO[PIO and DMA hardware]
    SPI --> Peripheral[SPI peripheral and GPIO CS]
    PIO --> Media[SD memory card]
    Peripheral --> Media
```

| Responsibility | Implementation |
| --- | --- |
| Board-approved pins and controller selection | [board.c](../firmware/src/board/board.c), [board_profile.c](../firmware/src/board/board_profile.c) |
| Singleton configuration, dispatch, and firmware/USB ownership | [sd_card.c](../firmware/src/drivers/sd_card/sd_card.c) |
| Block operations, result codes, and telemetry | [sd_card_block.h](../firmware/src/drivers/sd_card/sd_card_block.h) |
| CRC7, command framing, CMD8 classification, CSD decoding, serial CRC16, address conversion, and transfer bounds | [sd_card_protocol.c](../firmware/src/drivers/sd_card/sd_card_protocol.c) |
| Native card initialization and transfer policy | [sdio_pio.c](../firmware/src/drivers/sd_card/sdio_pio/sdio_pio.c) |
| Native response parsing and four-lane data encoding/CRC | [sdio_pio_protocol.c](../firmware/src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c) |
| PIO/DMA engine and wire timing | [sdio_pio_host.c](../firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.c), [sdio_pio_host.pio](../firmware/src/drivers/sd_card/sdio_pio/sdio_pio_host.pio) |
| SPI initialization and blocking transfers | [sd_spi.c](../firmware/src/drivers/sd_card/spi/sd_spi.c) |
| SPI response/token validation and pin-function checks | [sd_spi_protocol.c](../firmware/src/drivers/sd_card/spi/sd_spi_protocol.c) |
| FatFs result/status conversion and CTRL_SYNC | [diskio.c](../firmware/src/integration/fatfs/diskio.c) |

The facade links both backend libraries; each reuses the shared protocol library.
Filesystem mounting, formatting policy, and RTOS scheduling do not belong in
the reusable driver. The driver does not allocate its own task or heap buffers.

## Interfacing With The Driver

### Transport Selection

[config.h](../firmware/src/config/config.h) defaults to
`PF_CONFIG_SD_CARD_TRANSPORT_SDIO_PIO`. Select
`PF_CONFIG_SD_CARD_TRANSPORT_SPI` for SPI. Board support fills the corresponding
member of `pf_sd_card_config_t.bus`; pin assignments belong in board support,
not application code or reusable protocol helpers.

| Interface | Native SDIO-PIO | SD-SPI |
| --- | --- | --- |
| Configuration member | `bus.sdio_pio` | `bus.spi` |
| Configuration type | `pf_sdio_pio_config_t` | `pf_sd_spi_config_t` |
| Hardware selector | `pio0` or `pio1` | `spi0` or `spi1` |
| Backend API prefix | `pf_sdio_pio_` | `pf_sd_spi_` |
| Zero clock request | SYSCLK / 12, capped at 12.5 MHz | 25 MHz |
| Requested clock bounds | 400 kHz to 50 MHz, subject to host/card limits | 400 kHz to 25 MHz |

Transport and connection settings are locked after setup; repeating initialization
with the same configuration retries card identification. There is no public
runtime transport-switch or card-deinitialization API. Switching a card from SPI
back to native mode can require removing card power, not just resetting the MCU.

### Block API

Applications should normally use the facade rather than bypassing its ownership
gate with direct backend calls. This example initializes and reads without
mounting a filesystem or writing media:

```c
#include "board.h"
#include "sd_card.h"

pf_sd_card_result_t read_first_sector(uint8_t sector[PF_SD_CARD_BLOCK_SIZE])
{
    pf_sd_card_config_t config;

    if (!pf_board_get_sd_card_config(&config)) {
        return PF_SD_CARD_NOT_CONFIGURED;
    }
    pf_sd_card_result_t result = pf_sd_card_init(&config);
    if (result != PF_SD_CARD_OK) {
        return result;
    }
    return pf_sd_card_read_blocks(0u, sector, 1u);
}
```

For filesystem access, pass the board configuration to `pf_storage_mount()` and
use FatFs through the storage-owner workflow. Do not mix raw-sector writes with
a mounted filesystem.

- Blocks are always 512 bytes, with 32-bit sector addresses and capacity.
- Buffers are caller-owned and can have any alignment. Reserve at least
  `block_count * PF_SD_CARD_BLOCK_SIZE` bytes for the entire call.
- Nonempty requests require a non-NULL buffer, an in-capacity sector range, and
  a byte length representable by `size_t`. Zero-block calls are no-ops only
  after ownership/configuration/readiness checks.
- Calls are synchronous but not thread-safe. Serialize initialization, transfers,
  synchronization, telemetry access, and ownership changes; the owner flag is
  not a mutex.
- Use `pf_sd_card_sync()` after writes. A successful SDIO multi-block write can
  leave a CMD25 stream open; sync ends the stream and checks stop completion.
- On read failure, buffer contents may be partially updated or invalid. A failed
  write can have committed some sectors; do not blindly replay it.
- `pf_sd_card_get_info()` reports presence, initialization, effective clock,
  capacity, and CID. Without card detect, presence is not independently known.
- `pf_sd_card_set_idle_hook()` installs an optional short callback for long
  polling waits. Configure it before transfers; it is not synchronized and does
  not make all SDK calls nonblocking.

Result codes distinguish invalid arguments, locked configuration, unconfigured
transport, unready media/ownership, initialization failure, and I/O failure.
Neither initialization nor recovery formats media or writes card sectors.

## Initialization Workflow

Both transports begin identification at 400 kHz and determine whether sector
arguments are SDHC/SDXC block addresses or SDSC byte addresses.

```mermaid
flowchart TD
    Start[Board configuration] --> Validate[Validate pins, clock, and configuration lock]
    Validate --> Transport{Selected transport}
    Transport -->|SDIO-PIO| Claim[Claim two PIO state machines and one DMA channel]
    Claim --> NativeReset[CMD0 and CMD8]
    NativeReset --> NativeReady[CMD55 plus ACMD41 until ready]
    NativeReady --> Identify[CMD2 CID, CMD3 RCA, CMD9 CSD]
    Identify --> Select[CMD7 select and wait for DAT0 ready]
    Select --> Width[ACMD51 SCR, CMD13 status, ACMD6 four-bit mode]
    Width --> Length[CMD16 for byte-addressed cards]
    Length --> Speed[Decode CSD speed; negotiate CMD6 High Speed if requested]
    Speed --> Probe[Select clock and CRC-check up to 256 sectors]
    Probe --> Good{Probe accepted}
    Good -->|Yes| Ready[Initialized card and telemetry]
    Good -->|No, lower candidate available| NativeReset
    Transport -->|SPI| Setup[Configure SPI mode 0 and GPIO-controlled CS]
    Setup --> Clocks[Send 80 idle clocks with CS released]
    Clocks --> SPIReset[CMD0 and CMD8]
    SPIReset --> SPIReady[CMD55 plus ACMD41 until ready]
    SPIReady --> OCR[CMD58 OCR and address mode]
    OCR --> SPIIdentify[CMD16 if needed, CMD9 CSD, CMD10 CID]
    SPIIdentify --> SPISpeed[Set requested SPI baud rate, at most 25 MHz]
    SPISpeed --> Ready
```

Any command, resource, or validation failure returns an initialization error;
the diagram's native retry applies to failed transfer-clock qualification, not
every identification failure. Native mode requires SCR-advertised four-bit
support; there is no one-bit sector-I/O fallback. SPI accepts its supported
ready/idle responses and checks OCR power-up and voltage compatibility.

Native clock candidates are 50, 40, 33, 25, 20, 10, 5, and 1 MHz. Candidates
above the requested/card/recovery limit or unsupported by SYSCLK are skipped.
Above 25 MHz, CMD6 must confirm High Speed; otherwise normal speed is used.
Host divider rounding can make the actual rate lower than the candidate.
Fast-mode requests require SYSCLK of at least four times the requested bus rate.
Use SDK/board-supported clock settings; accepting a divider is not qualification
of an overclocked MCU.

The read-only probe CRC-checks and discards at most 256 sectors in batches of
at most 64, without allocating a separate payload buffer. It
rejects a candidate if recovery lowers the clock, and above 20 MHz also requires
working multi-block reads. A failed qualification restarts identification at
400 kHz with a lower limit. This short probe does not qualify writes, signal
integrity margins, endurance, or power-loss safety.

## Read Workflow

```mermaid
flowchart TD
    Request[Read request] --> Check[Check owner, readiness, buffer, and range]
    Check --> Mode{Transport}
    Mode -->|SDIO-PIO| Close[Close any pending CMD25 write stream]
    Close --> Batch{Multi-block receive available}
    Batch -->|Yes| CMD18[CMD18 and DMA batches of at most 64 sectors]
    CMD18 --> Decode[Decode completed 520-byte wire images while DMA receives later blocks]
    Decode --> CRC[Check four lane CRC16s and issue CMD12 stop]
    CRC --> NativeOK{Read outcome}
    NativeOK -->|Yes| Done[Return result]
    NativeOK -->|Valid bulk rejection and clean stop| Single[Disable batching for session and retry via CMD17]
    NativeOK -->|CRC, timeout, uncertain response, or stop failure| Recover[Reinitialize at 400 kHz with lower clock limit]
    Batch -->|No| Single
    Single --> Retry{CRC-checked read successful}
    Retry -->|Yes| Done
    Retry -->|No| Recover
    Recover --> Identity{Presence, CID, and capacity still match}
    Identity -->|Yes, candidate available| Batch
    Identity -->|No or exhausted| Error[Return error]
    Mode -->|SPI| SPIRead[CMD18 for multi-sector requests or CMD17 for one sector]
    SPIRead --> SPICRC[Wait for data token, read payload, check serial CRC16]
    SPICRC --> SPIStop[CMD12 and busy wait after CMD18; release CS]
    SPIStop --> Done
```

The native host falls back to CMD17 only after a valid bulk rejection and a
successful stop. CRC errors, timeouts, missing/malformed responses, and failed
stops return to card-layer recovery without latching batching off. The card
layer performs clock-limited recovery if a read fails.
Recovered reads retain the lower clock. Identity checks prevent a retry from
silently reading a replacement card. SPI holds CS across a CMD18 transaction,
stops with CMD12, discards CMD12's stuff byte, and waits for ready before release.
An explicit illegal-command response permits single-sector fallback; other
failures are not replayed. SPI does not implement the native clock-recovery ladder.

## Write Workflow

```mermaid
flowchart TD
    Request[Write request] --> Check[Check owner, readiness, buffer, and range]
    Check --> Mode{Transport}
    Mode -->|SDIO-PIO| Continue{Next sector of an open CMD25 stream}
    Continue -->|Yes| Stream[Continue CMD25 without closing]
    Continue -->|No| Close[Close old stream if present]
    Close --> Count{More than one sector}
    Count -->|Yes| Open[Issue CMD25 and switch PIO to clock plus TX programs]
    Open --> Stream
    Stream --> Encode[Alternate two wire images; encode next block during current DMA and busy wait]
    Encode --> Accept[Check card data-response token and DAT0 busy release]
    Accept --> Pending[Return success with stream open at next sector]
    Pending --> Sync[Sync, read, noncontiguous write, reinit, or removal closes stream]
    Sync --> Stop[Restore command mode and send CMD12; lower-clock stop retry if needed]
    Count -->|No| Single[CMD24, one DMA wire image, response and busy wait]
    Single --> Restore[Restore command and RX programs]
    Mode -->|SPI| SPIWrite[CMD25 for multi-sector requests or CMD24 for one sector; hold CS]
    SPIWrite --> Serial[Send payload and CRC16; check response and wait until ready]
    Restore --> Done[Return result]
    Serial --> SPIStop[After CMD25 send stop token and wait for ready; release CS]
    SPIStop --> Done
    Stop --> Done
```

DMA byte-swaps 32-bit words so each 520-byte native wire image reaches the PIO
in transmission order. PIO sends data/CRC nibbles, captures the write-response
token, and waits for card busy release. Multi-block writes overlap CPU encoding
of the next image with transmission/programming of the current sector, but use
only one DMA channel. Failed writes are not automatically replayed. Stop-command
recovery is not a guarantee against data loss on power failure.

SPI multi-block writes use the 0xFC block token and 0xFD stop token. They are
closed within each call, not kept open across calls. Illegal-command fallback
is allowed only before sending payload; partial writes and failed stop/busy
waits return errors without replay. A failed stop invalidates initialization.

## Hardware Resource Inventory

These are driver resources, not the complete firmware budget. UART logging,
USB hardware, RTOS tasks/queues, and FatFs working memory belong to their
respective integrations. Both transports need a compatible 3.3 V card supply,
common ground, and the breakout's required biasing.

### Current Board Profile

The source profile selects SDIO-PIO by default and requests 50 MHz. This is a
request, not a guaranteed effective clock; identification and qualification can
select a lower rate. SPI's zero clock request selects 25 MHz. See
[board_profile.c](../firmware/src/board/board_profile.c) for authoritative values
and [hardware setup](hardware-setup.md#memory-and-storage-connections) for header
pin numbers and electrical details.

| Resource | SDIO-PIO | SD-SPI |
| --- | --- | --- |
| Controller | PIO0 | SPI0 |
| GP2 | CLK | SCK |
| GP3 | CMD | MOSI / card CMD |
| GP4 | DAT0 | MISO / card DAT0 |
| GP5 | DAT1 | No driver signal; remains connected/reserved by wiring |
| GP6 | DAT2 | No driver signal; remains connected/reserved by wiring |
| GP7 | DAT3 | GPIO-controlled CS / card DAT3 |
| GP8 | Card detect | Card detect |
| Detect polarity and bias | Active high, breakout bias, no MCU pull | Same |

The two columns are alternative configurations, not concurrently active buses.
Native DAT0-DAT3 must be ascending consecutive GPIOs in logical lane order.
The current PIO clock-wait encoding requires CLK = DAT0 - 2; DAT0 must be in
the validated GP2-GP28 range and all used pins must exist below GP32. CMD and
optional detect must not overlap the bus pins. The backend accepts PIO0/PIO1,
not RP2350 PIO2. SPI SCK/MOSI/MISO must match functions of the selected SPI
controller; CS is an ordinary, independently controlled GPIO.

### Peripheral And Memory Budget

| Resource | SDIO-PIO allocation/use | SD-SPI allocation/use |
| --- | --- | --- |
| Bus GPIOs | Six: CLK, CMD, DAT0-DAT3 | Four: SCK, MOSI, MISO, CS |
| Optional detect GPIO | One, polled input | One, polled input |
| PIO state machines | Two dynamically claimed on one PIO: command/clock SM and data RX/TX SM | None |
| PIO instruction memory | Selected PIO's 32-word budget reserved for this host; programs replaced between modes | None |
| PIO programs in command/read mode | Command program plus normal or fast RX program | None |
| PIO programs in write mode | Clock program plus normal or fast TX program; same two SMs | None |
| PIO FIFO resources | TX/RX FIFOs of both claimed SMs; data FIFO supplies DMA DREQ | None |
| PIO completion flags | Internal flags 0: command, 1: receive, 2: transmit; polled by CPU | None |
| CPU interrupt handlers | Default: none. Optional FreeRTOS adapter uses one shared handler on the selected PIO's IRQ0; no DMA IRQ handler | None |
| DMA channels | One dynamically claimed channel, reused for RX and TX with 32-bit transfers and byte swap | None; CPU uses blocking SDK SPI calls |
| SPI peripheral | None | One controller, 8-bit mode 0, MSB first; GPIO CS spans command, data, and busy phases |
| GPIO input synchronization | Bypassed for the six native bus pins in selected PIO; previous settings saved/restored by host teardown | No PIO synchronization changes |
| Internal pulls | CMD and all four DAT pins pulled up | MISO pulled up; CS initialized high |
| Clock source | `clk_sys` drives SM dividers; effective rate is reported in telemetry | SDK SPI baud-rate configuration; actual rate is reported |
| Timing source | SDK microsecond timer for bounded polling; SDK sleeps during identification | Same; no dedicated timer/alarm claimed |
| Static receive wire buffer | 64 x 130 words x 4 bytes = 33,280 bytes | None |
| Static initialization probe buffer | None; CRC-only reads reuse the receive wire buffer | None |
| Main static native transfer buffer | 33,280 bytes total, excluding metadata | None |
| Single-sector wire image | 520 bytes on stack for native single-block read/write paths | No full-sector bounce buffer |
| Multi-block write wire images | Two x 520 bytes = 1,040 bytes on stack | None |
| Other stack/static data | SCR/switch-status buffers, command responses, host/configuration state, CID and capacity; additional stack beyond images required | Small command/CSD/CID buffers and configuration/telemetry state |
| Protocol lookup tables | 8,192 bytes of lazily initialized SRAM tables for four-lane CRC, plus the shared read-only CRC16 table; software CRC | Shared read-only CRC16 table; software CRC |
| Main native buffer plus CRC tables | 41,472 bytes (40.5 KiB), excluding metadata and stack; the separate self-test buffer adds 32 KiB | No native wire/CRC-slice storage used by SPI operations |
| Caller transfer buffers | Caller owns logical payload storage independently of wire/probe buffers | Caller owns logical payload storage |
| Core/task/heap allocation | No dedicated core, driver task, or heap allocation | Same |

SM and DMA numbers are allocated at runtime, not fixed to SM0/SM1 or DMA0.
The selected PIO should not be shared with unrelated programs: mode switching
depends on available instruction memory and owns the listed completion flags.
Resource-allocation failure returns initialization failure; internal host teardown
unclaims its DMA/SMs and removes its programs. The public singleton retains its
connection configuration and does not expose general resource hand-back.
The facade still links both transports: unused native buffers may remain in a
SPI firmware image, depending on linker reachability. An inactive transport
does not claim its PIO/SM/DMA hardware. Use the link map for total image SRAM,
not just this active-transport table.

## Performance Controls And Status

Profiling is disabled at boot. The storage-owner CLI supports `sd profile on`,
`off`, `reset`, and `status` (or just `sd profile`). Snapshots report cumulative
microseconds/calls for command, data, CRC/copy, busy, stop, and recovery work,
plus CRC failures, batch fallbacks, timeouts, and recovery attempts. Changes
require firmware ownership. Profiling adds measurement overhead; phases overlap
and must not be summed to infer total latency. Native write data timing includes
overlapped next-block encoding and the PIO's response/busy completion; it does
not isolate the card's internal programming time. Snapshots use
`pf_sd_card_get_profile()`; callers must serialize access just like block I/O.

| Review improvement | Current status |
| --- | --- |
| Phase profiling and recovery diagnostics | Implemented, disabled by default; CLI and C API available |
| Preserve batching after transient faults | Implemented; CRC/timeout/uncertain-stop failures enter bounded card recovery |
| Remove initialization-only payload storage | Implemented; saves 32 KiB, retaining probe coverage and CRC checks |
| Fuse CRC and payload copying | Opt-in `PF_SDIO_PIO_FUSED_COPY` CMake option, OFF by default; both variants host-tested; target speed unmeasured |
| Interrupt-backed synchronous native waits | Opt-in `PF_SD_CARD_INTERRUPT_WAIT`, OFF by default; adapter and notification races host-tested; target scheduling unqualified |
| SPI CMD18/CMD25 | Implemented with CS, stop, busy, and no-write-replay checks; hardware qualification outstanding |
| Contiguous-file comparison | Opt-in `PF_STORAGE_SELF_TEST_PREALLOCATE`, OFF by default; see [SD self-test](sd-self-test.md) |
| Chained native receive buffers | Not implemented: enable only after profiles demonstrate meaningful inter-batch gaps, with overflow/cancellation/stop tests and an SRAM/DMA budget |
| SPI bulk DMA | Not implemented: evaluate after CMD18/CMD25 measurements show CPU-bound payload transfers; requires RX/TX servicing and a defined completion/abort policy |

The optional native wait integration lives in
[sd_card_wait.c](../firmware/src/drivers/sd_card/freertos/sd_card_wait.c). It reserves
task-notification slot 1 (slot 0 remains available), enables only the active data
completion source, and blocks for at most one RTOS tick per wait, plus scheduler
latency. It masks the IRQ source but leaves the PIO flag for the driver to clear.
The host still requires both PIO completion and DMA completion. Boot without a
running scheduler retains polling. This adapter supports the current serialized,
single-core runtime, not concurrent card callers or a second-core owner.

Measure each option separately at the same supported clock, card, filesystem,
file size, and timing boundary. Record effective clocks, error/recovery counts,
CRC integrity, SRAM and stack high-water marks, CPU availability, and latency,
not only average MB/s. Keep normal and preallocated filesystem results separate.
CRC checks and bounded cleanup remain mandatory. Chained DMA and a faster bus
are not assumed improvements, and no new hardware throughput claim is made.

## Ownership Workflow

```mermaid
sequenceDiagram
    participant FW as Storage owner task
    participant Card as SD-card facade
    participant USB as USB/MSC integration
    participant Host as USB host
    FW->>Card: Firmware block operations and sync
    FW->>FW: Finish work, close files, unmount filesystem
    USB->>Card: Set owner to USB MSC after integration checks
    Host->>USB: Sector read/write or synchronize
    USB->>Card: pf_sd_card_msc_read/write_blocks or msc_sync
    Note over FW,Card: Firmware block operations return NOT_READY during MSC ownership
    Host->>USB: Safely eject and allow removal
    USB->>Card: Set owner back to firmware
    Card-->>FW: Reinitialization request available
    FW->>FW: Invalidate cached filesystem state and reinitialize before remount
```

The facade enforces which client may issue block operations; it does not itself
close files, unmount FatFs, wait for host eject, or serialize tasks. Those checks
belong to storage/USB integration. Returning ownership sets a reinitialization
request because the host may have changed the filesystem. See
[USB protocol and MSC lifecycle](usb-protocol.md) for transition rules.

## Validation And Limits

Host checks cover shared protocol arithmetic, native wire encoding/CRC, SPI
tokens, native initialization/recovery/transfer routing, DMA batch decoding,
and PIO timing models. Relevant checks live in
[firmware SD-card tests](../firmware/tests/drivers/sd_card),
[check-sdio-init.py](../tools/validation/check-sdio-init.py),
[check-sdio-read-batch.py](../tools/validation/check-sdio-read-batch.py),
[check-sd-spi-transfer.py](../tools/validation/check-sd-spi-transfer.py), and
[check-sdio-pio-timing.py](../tools/validation/check-sdio-pio-timing.py).
Use the existing [test guidance](../firmware/tests/README.md) and
[project validation entry point](../tools/validation/validate.sh).

Host tests and timing models do not establish electrical margins or target
hardware safety. Physical qualification is card, board, wiring, and clock
specific; see [hardware records](hardware-setup.md#sd-card-breakout),
[SD self-test](sd-self-test.md), and [test records](tests/records).
Read-only initialization success does not qualify high-speed writes. Power-loss
recovery, complete MSC lifecycles, and sustained endurance require separate
qualification. CSD v1/v2 capacity must fit the 32-bit block contract; SDUC,
64-bit LBAs, asynchronous requests, and a one-bit native block backend are not
implemented.

### Previously Recorded Qualification

These observations were previously recorded in the architecture guide. They
are retained as historical, configuration-specific results, not as qualification
of this documentation change or every compatible card:

| Card/configuration | Recorded result |
| --- | --- |
| 8 GB Class 10, 250 MHz SYSCLK | 32 MiB self-test exercised lower-clock CMD12 retry, completed near 33 MHz with zero mismatches; 8.48 MB/s write and 9.91 MB/s read |
| 16 GB Class 4, 20 MHz request / 18.75 MHz actual | 32 MiB passed; 7.11 MB/s write and 6.69 MB/s read |
| 8 GB SD8GB, stock 150 MHz SYSCLK / 25 MHz SDIO | 32 MiB passed with zero mismatches; 7.22 MB/s write and 1.45 MB/s read |
| 128 GB SN128, 250 MHz SYSCLK / 50 MHz SDIO | 32 and 128 MiB self-tests recorded zero mismatches; 15.9 MB/s write and 13.7 MB/s read |
| 128 GB SN128, 200 MHz SYSCLK / 50 MHz SDIO | Recorded 12.5 MB/s write and 9.5 MB/s read |
| 128 GB SN128, stock 150 MHz SYSCLK / 25 MHz SDIO | Recorded 9.5 MB/s write and 8.3 MB/s read |
| 128 GB SN128, 280 MHz SYSCLK | Did not work with the card even at 25 MHz SDIO |

High-speed RX uses falling-edge sampling; its data divider is half the
command/clock divider with matched fractional timing. High-speed TX starts
the clock and data state machines together and launches at a fixed cycle offset.
The slow TX program is not suitable for 50 MHz: its observed-edge launch left
insufficient setup time and produced card CRC-error tokens. A 50 MHz bus needs
at least 200 MHz SYSCLK; those Pico 2 overclock experiments are diagnostic,
not supported production clock settings. The former 64 MiB initialization scan
could delay startup for minutes with single-sector fallback; the current probe
is bounded to 128 KiB.