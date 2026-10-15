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

The build selects one backend; it reuses the shared protocol library. SPI-only
builds omit native SDIO code, including its 33,280-byte receive buffer and 8 KiB
CRC tables. Filesystem mounting, formatting policy, and RTOS scheduling do not
belong in the reusable driver. The driver does not allocate its own task or
heap buffers.
USB starts before card initialization. The FreeRTOS wait adapter is installed
before the storage worker is created; that worker owns the single card-init and
mount path, so a slow or absent card does not delay USB enumeration.

## Interfacing With The Driver

### Transport Selection

The CMake cache variable `PF_SD_CARD_TRANSPORT` selects `SDIO_PIO` (default) or
`SPI`; the build defines `PF_CONFIG_SD_CARD_TRANSPORT` consistently for board,
facade, and backend code. Use `tools/firmware/build.sh --sd-card-transport SPI`
or `-DPF_SD_CARD_TRANSPORT=SPI` for a direct CMake build. Board support fills
the selected member of `pf_sd_card_config_t.bus`; pin assignments belong in
board support, not application code or reusable protocol helpers.

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
- Facade calls are synchronous and individually serialized by a recursive
  FreeRTOS mutex. Direct SPI/SDIO backend and protocol calls bypass that mutex
  and require external serialization, including lazy CRC-table initialization.
  The owner flag controls access eligibility; it does not reserve a sequence of calls.
- Reserve multi-call check/action sequences with `pf_sd_card_lock()` and
  `pf_sd_card_unlock()` where needed. Per-call locking does not serialize a
  FatFs transaction: `FF_FS_REENTRANT` is disabled, and all FatFs operations
  must remain in the storage-owner workflow with its busy/ownership reservation.
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

Native clock candidates are 50, 40, 33, 25, 20, 10, 5, and 1 MHz, then
400 kHz. Requests from 400 kHz through 999,999 Hz select the 400 kHz
candidate and run the same CRC-checked read probe; requests below 400 kHz
are rejected. Candidates
above the requested/card/recovery limit or unsupported by SYSCLK are skipped.
Above 25 MHz, CMD6 must confirm High Speed; otherwise normal speed is used.
Host divider rounding can make the actual rate lower than the candidate. The
slow native PIO programs require SYSCLK of at least four times the selected
effective bus rate; the requested clock is only an upper bound and may be
reduced to a supported candidate.
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

The PIO programs have no timeouts of their own: the command, data, and busy
waits are bounded by the host (100 ms for a command response, 1 s for data
transfer and for DAT0 busy release), after which the state machines are
restarted. A command that gets no response costs 100 ms; this affects only
initialization and failure paths. Native legacy compatibility currently requires
an intact, CRC-valid CMD8 illegal-command response. A silent legacy card is
rejected: the host's boolean completion result cannot distinguish silence from a
partial frame, so neither timeout nor CRC/framing failure is accepted as legacy.
A
command that completes before a delayed poll is not reported as a timeout.
In-PIO timeouts are not used because the command (19) and receive (11) programs
leave only two of the 32 PIO instructions free.

## Write Workflow

A failed native single-sector write invalidates readiness, even if restoring the
host's command programs succeeds. CMD24 response loss, interrupted payload, and
DAT0 timeout cannot prove the card completed the write. Sync and subsequent
transfers return `NOT_READY` until explicit reinitialization; no payload is
replayed automatically. This conservatively also invalidates ordinary rejection.

CMD24 and CMD25 share TX DMA control-bit setup. CMD25 constructs those invariant
bits once per call; payload DMA addresses/counts and completion/abort handling
remain per sector. This reduces setup work but has no measured throughput claim.

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
    Pending --> Sync[Sync, read, noncontiguous write, or reinit closes stream]
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

A failed native multi-block write returns an error without replay. Initialization
is invalidated when cleanup leaves the CMD25 stream unresolved (command-mode
restoration or CMD12 response/busy recovery failed). If cleanup succeeds, the
card remains initialized; this does not mean the failed write was atomic or that
no data reached the card. Earlier accepted blocks, and possibly the failing block,
may already be programmed. CMD12 terminates the stream; it is not the first point
at which blocks can reach media. In SDIO, ordinary single-block card rejection does not
invalidate initialization when command-mode restoration succeeds. In SPI, a
single-block command that returns a non-zero R1 keeps initialization (the card
is known to be in command mode), but a lost R1 or any failed read data phase
(token timeout/error, short read, CRC mismatch) invalidates it because the
card's transfer state cannot be proven restored; failed writes follow the rules
above. Recovery is reinitialization; failed transfers are not retried. Failed host
restoration explicitly invalidates readiness, including after a single-block write;
sync cannot report success. `pf_sdio_pio_host_recover()` resolves both pending
streams and host restoration failure. Its false result retains either unresolved
stream state or the explicit unusable-host flag. Reinitialization retries restoration
before identification, without replaying the uncertain write. Single-sector and
CMD6 reads share RX setup/cleanup and response checks; SCR keeps its one-bit CRC
and command sequence.

Raw card-detect absence immediately invalidates readiness and blocks transfers,
sync, and initialization, while preserving unresolved protocol state. A bounce
back requires reinitialization to close that state before CMD0. Only the serialized
storage worker's debounced removal notification abandons streams and restores host
mode without sending CMD12 to the absent card. This notification precedes unmount.
The worker continues presence debounce under USB ownership, but never initializes
or mounts while USB owns the card. MSC disable permits release before host eject
only when the worker has confirmed absence and raw detect still reports absence;
removal prevention still blocks release. Present media still requires eject and
successful sync. Ownership return requests reinitialization, and debounced
reinsertion permits the firmware worker to initialize and mount again.

CMD18 response timeout or malformed response does not prove the command was never
sent. Every validated multi-block read attempt cleans up RX/DMA and attempts CMD12,
including those response failures. A failed stop retains an unresolved read flag;
the host rejects new reads and non-stop commands. Card-layer recovery first closes
that state successfully before identification or retry. Exhausted stop recovery
leaves initialization invalid and subsequent I/O blocked. A confirmed removal may
abandon this state without a card command.

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

The default CMake transport is SDIO-PIO; the board profile requests 20 MHz (18.75 MHz at 150 MHz SYSCLK with the host divider).
This is a request, not a guaranteed effective clock; identification and
qualification can select a lower rate. SPI's zero clock request selects 25 MHz. See
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
| Static native receive/transfer wire buffer | 64 x 130 words x 4 bytes = 33,280 bytes | None |
| Static initialization probe buffer | None; CRC-only reads reuse the receive wire buffer | None |
| Single-sector wire image | 520 bytes on stack for native single-block read/write paths | No full-sector bounce buffer |
| Multi-block write wire images | Two x 520 bytes = 1,040 bytes on stack | None |
| Other stack/static data | SCR/switch-status buffers, command responses, host/configuration state, CID and capacity; additional stack beyond images required | Small command/CSD/CID buffers and configuration/telemetry state |
| Protocol lookup tables | 8,192 bytes of lazily initialized SRAM tables for four-lane CRC, plus the shared read-only CRC16 table; software CRC | Shared read-only CRC16 table; software CRC |
| Main native buffer plus CRC tables | 41,472 bytes (40.5 KiB), excluding metadata and stack; the separate self-test buffer adds 32 KiB | SPI-only builds omit these 40.5 KiB of native SRAM |
| Caller transfer buffers | Caller owns logical payload storage independently of wire/probe buffers | Caller owns logical payload storage |
| Core/task/heap allocation | No dedicated core, driver task, or heap allocation | Same |

SM and DMA numbers are allocated at runtime, not fixed to SM0/SM1 or DMA0.
The selected PIO should not be shared with unrelated programs: mode switching
depends on available instruction memory and owns the listed completion flags.
Resource-allocation failure returns initialization failure; internal host teardown
unclaims its DMA/SMs and removes its programs. The public singleton retains its
connection configuration and does not expose general resource hand-back.
Only the selected backend is linked, so an SPI-only image omits the native
wire buffer and CRC tables. Use the link map for total image SRAM, not just
this active-transport table.

## Performance Controls And Status

Profiling is disabled at boot. The storage-owner CLI supports `sd profile on`,
`off`, `reset`, and `status` (or just `sd profile`). Snapshots report cumulative
microseconds/calls for command, data, CRC/copy, busy, stop, and recovery work,
plus CRC failures, batch fallbacks, timeouts, and recovery attempts. Changes
require firmware ownership. Profiling adds measurement overhead; phases overlap
and must not be summed to infer total latency. Native write data timing includes
overlapped next-block encoding and the PIO's response/busy completion; it does
not isolate the card's internal programming time. SPI read DATA includes token
waiting, payload transfer and both wire CRC bytes; CRC covers CPU validation
separately. A token deadline emits one timeout event; I/O failures do not. Snapshots use
`pf_sd_card_get_profile()`; the facade locks each snapshot, while direct backend
profiling access requires external serialization.

### Profile Measurement Boundaries

The existing enum order, `elapsed_us`/`calls` arrays and CLI labels
`command`, `data`, `crc_copy`, `busy`, `stop`, `recovery` are retained for ABI
and historical log compatibility. They are **inclusive instrumentation windows**,
not mutually exclusive stages. Reinterpreting them as exclusive phases would
silently change existing results; no such reinterpretation is made.

| Window | Current boundary and overlap |
| --- | --- |
| COMMAND | Command request/response helper; also counted inside STOP and recovery initialization |
| SDIO CMD24 DATA | After TX starts, until both DMA and PIO completion (or timeout); PIO completion includes response token and DAT0 busy release; no separate BUSY measurement for that program |
| SDIO CMD25 DATA | Per-block interval after TX starts through both completions; includes next-block CPU encoding, measured again under CRC/copy; also includes response and card busy |
| SDIO RX DATA | CMD18 response accepted through DMA + PIO completion/timeout; completed-block decode/CRC/copy inside the wait is counted again as CRC/copy; final decode after completion lies outside DATA |
| SPI DATA | Read token wait + payload + wire CRC; write payload only, excluding response and the separately timed ready wait |
| CRC/copy | CPU encode/decode/CRC validation windows; may run while DMA/PIO is transferring; includes copy only for the native variants that perform it |
| BUSY | Explicit ready wait; native TX PIO busy is already in DATA; explicit stop waits are also inside STOP |
| STOP | Entire multiblock stop helper including command/response and busy; native clock-fallback attempts included |
| RECOVERY | Native reinitialization attempt, including its command/data/CRC/busy windows; not all cleanup or retry time |

Do not sum these values to obtain latency, subtract CRC/copy to infer physical
wire time, or compare individual windows across transports. The CMD25 host
regression uses real production encoding with a deterministic clock to assert
nested DATA=119 ticks and CRC/copy=51 ticks for three blocks; these numbers are
instrumentation evidence, not hardware microseconds. Existing CMD24 regression
checks both completion sources and timeout accounting. RX decode and stop/busy
boundaries above were source-reviewed; they have no new hardware timing evidence.

### MSC Batching And Validation Boundary

Pinned TinyUSB 0.18.0 `msc_device.c` uses
`min(CFG_TUD_MSC_EP_BUFSIZE, remaining_command_bytes)` for READ10 and WRITE10
staging; WRITE10 passes the actual received byte count. This repository sets
`CFG_TUD_MSC_EP_BUFSIZE=512`. A SCSI command can contain multiple sectors, but
its current callbacks carry at most one sector, with offsets for short transfers.
The callback interface itself accepts larger buffers; complete 512-byte multiples
at offset zero now reach the existing block backend as one count-preserving call.
Partial calls must fit one sector and use synchronous RMW; zero-byte valid calls
perform no I/O. Size is bounded by signed callback return range and card capacity.

MSC reserves the recursive facade lock across validation, RMW and completion;
firmware/FatFs I/O remains excluded by USB ownership. Diskio validates drive,
nonzero count and address width and forwards multi-sector firmware requests;
transport validation enforces card capacity. Both transports accept unaligned
caller buffers: native SDIO encodes into the existing aligned double TX workspace
and decodes from its bounded RX workspace; SPI blocking I/O consumes bytes.
MSC retains only its existing aligned single-sector RMW scratch, with no new
allocation, DMA buffer or cached callback pointer.

Native multi-block writes can leave CMD25 open after accepted/busy-complete
blocks. MSC therefore checks sync/STOP after a successful multi-block backend
write and before returning success. A failed write is never replayed or synced
as a recovery attempt. Sync errors return failure and write-error sense. Current
one-sector callbacks retain their existing synchronous completion behavior.

Cross-callback write aggregation is not implemented: TinyUSB's completion hook
runs after CSW is queued, too late to fail the command on a flush error, and the
application has no pre-CSW command boundary with total write input validity.
Acknowledging cached data early would weaken the error/durability contract.
Current 512-byte BOT staging remains a performance ceiling; no USB throughput
gain is claimed from accepting larger callback inputs.

`check-msc-batch.py` executes production MSC, facade and diskio against a
data-bearing backend seam (actual bytes, ownership, RMW, batch counts, sense and
checked completion). It does not execute TinyUSB's BOT state machine or physical
SD transport. Existing SDK TinyUSB tests require an external test environment;
there is no configured runnable full-stack host BOT integration in this repository.
BOT reset/short-transfer/CSW error integration and real USB-to-card completion
remain HIL validation gaps, including the review's GIL/integration gap; no Python
GIL behavior is inferred from these C host tests. No dependencies were installed.

For a future authorized controlled measurement, use the same board/card,
firmware options, clock and contiguous LBA range on both revisions; record
artifact hashes and actual bus clock, SCSI command sector counts and observed
backend counts. Reset/enable profiles per run; record total wall time and bytes
separately from inclusive counters, repeat read/write runs with matched sync and
cache policy, and report distributions/errors. Changing staging requires its own
BOT/short-input/abort validation before an A/B performance claim. Do not run this
recipe on media without separate explicit authorization.

The historical target records identify earlier artifacts, not this remediation.
Software models cannot measure PIO/DMA scheduling, command gaps, or card busy time.
No controlled before/after benchmark or HIL run is available for these changes,
including the new low-end candidate. No hardware qualification or throughput gain
is claimed. Profile command gaps, CRC/copy, filesystem allocation (normal versus
preallocated files), and card busy time before choosing further optimizations.

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
close files, unmount FatFs, or wait for host eject. Those checks belong to
storage/USB integration. A statically allocated FreeRTOS recursive mutex now
serializes all facade operations for both transports, including telemetry and
removal cleanup. Presence service and ownership handoff reserve their whole
check/action sequences; reconnect delays and command replies run outside the
reservation. These reservations protect those specific sequences, not every
FatFs transaction. The storage worker's exclusive workflow and busy gating
protect filesystem operations across multiple facade calls; the per-call mutex
alone cannot do that. Profile snapshots use the same mutex. Returning ownership sets a reinitialization
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

All native RX programs sample data on the falling clock edge (mid-bit), and the
slow TX CRC-status read does the same; the busy check runs one bit after the end
bit. This replaces sampling one cycle after the rising edge, which is inside the
card's 2.5-14 ns output-change window. High-speed RX uses a three-instruction
loop; its data divider is half the
command/clock divider with matched fractional timing. High-speed TX starts
the clock and data state machines together and launches at a fixed cycle offset.
The slow TX program is not suitable for 50 MHz: its observed-edge launch left
insufficient setup time and produced card CRC-error tokens. A 50 MHz bus needs
at least 200 MHz SYSCLK; those Pico 2 overclock experiments are diagnostic,
not supported production clock settings. The former 64 MiB initialization scan
could delay startup for minutes with single-sector fallback; the current probe
is bounded to 128 KiB.

## Static SD Review Follow-up (2026-10-15)

Application READ10/WRITE10 cached sense is now invalidated after TinyUSB handles
an accepted BOT reset at SETUP, and after its MSC driver bus-reset processing.
GNU linker wrappers in `firmware/src/usb/msc.c` interpose the built-in driver's
external references from `usbd.c`; the vendored stack is unchanged. No ISR or
enqueue-time hook mutates this cache. Earlier queued failures cannot erase a
reset, and failures after handled reset retain their fresh sense. Rejected
requests, skipped SETUP events, and DATA/ACK stages do not clear it.

`python3 tools/validation/check-sd-msc-production.py` checks pinned
source ordering and runs production application callbacks and linker wrappers
against a separate host driver/queue model. TinyUSB itself is not linked in that
host regression; it is not a real BOT integration or hardware qualification.

At that reset-sense review, the data paths were unchanged: SDIO CMD18 already overlaps completed-block
decode/CRC with later RX DMA, CMD25 alternates two wire buffers while preparing
the next block during TX, and fused CRC/copy remains opt-in. FatFs passes
multi-sector requests through and SPI already uses CMD18/CMD25. No safe small
performance simplification was identified in that review. The later MSC callback
batch-boundary repair is described above; the underlying overlap remains. Phase profiling and same-card,
same-clock normal/preallocated and fused-copy A/B comparisons remain pending
HIL; this change makes no throughput claim and changes no clocks or buffers.
