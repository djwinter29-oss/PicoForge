# PicoForge USB Status Protocol

## Device Interfaces

The development device uses VID/PID `0xCAFE:0x4011` in both configurations. Replace this unallocated development identity before product release. The HID interface remains interface 2; when MSC is enabled, its interface is appended as interface 3.

- CDC ACM is the interactive [Embedded CLI console](cdc-cli.md); the prompt appears after the host asserts DTR. UART debug logs remain separate.
- HID uses vendor usage page `0xFF00`, usage `0x01`.
- USB product string: `PicoForge CDC + HID`.
- USB serial string: the Pico SDK unique board ID.

The HID report ID is the first byte of each report delivered by host APIs. Multi-byte integer fields are little-endian. Feature-report APIs may prepend an additional zero report-number byte on some Linux transports; host decoders accept that transport prefix and zero-filled report padding. Hosts read Feature Report 1 once when connecting for stable identity, then use periodic Input Reports 2 and 3 for changing values.

## Feature Report 1

Feature report 1 has a 42-byte payload, excluding the report ID. Its layout version is `2`.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `2` |
| 1 | 1 | MCU ID | `1` = RP2040, `2` = RP2350 |
| 2 | 20 | Product name | NUL-padded ASCII, currently `PicoForge` |
| 22 | 16 | USB serial | ASCII hexadecimal unique board ID; same value as the USB serial string |
| 38 | 4 | Reserved | Zero-filled; reserved for a future software-version field |

All identity fields are stable for a connection. `clk_sys` is intentionally excluded because firmware may change it at runtime.

## Input Report 2

Input report 2 has a 13-byte payload and is published every 250 ms. Its layout version is `2`.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `2` |
| 1 | 1 | Sequence | Increments for each accepted periodic report |
| 2 | 4 | Uptime | Unsigned milliseconds, little-endian; wraps after `2^32` ms |
| 6 | 4 | `clk_sys` | Current unsigned frequency in Hz, little-endian |
| 10 | 1 | Flags | Bit 0: USB mounted; bit 1: CDC DTR/open; bit 2: temperature valid; bit 3: board LED available; bit 4: board LED on; other bits reserved |
| 11 | 2 | Temperature | Signed centidegrees Celsius, little-endian; meaningful only when bit 2 is set |

The sequence byte wraps modulo 256. Temperature comes from the MCU's internal sensor and is an estimate, not a calibrated ambient measurement. If bit 4 is set, bit 3 must also be set. The input report supports `GET_REPORT`, returning current telemetry. The current system clock is sampled for each report, so hosts should treat it as dynamic.

## Input Report 3

Input report 3 has a 29-byte payload and is normally published every 1000 ms.
Its layout version is `2`. The storage worker initializes and mounts detected
media according to [the mounting policy](cdc-cli.md). If storage holds the card
reservation, periodic memory reports are deferred and GET_REPORT returns no
report rather than blocking USB on card initialization/recovery. Hosts should
retry; no stale or fabricated memory snapshot is sent.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `2` |
| 1 | 1 | Sequence | Increments for each accepted memory report |
| 2 | 1 | Flags | Bit 0: configured; bit 1: presence known; bit 2: present; bit 3: initialized; bit 4: CID valid; bit 5: capacity valid; other bits reserved |
| 3 | 1 | Component type | `1` = SD card; `0` = unavailable |
| 4 | 1 | Bus mode | `0` = unknown; `1` = SDIO-PIO; `2` = SPI |
| 5 | 4 | Bus clock | Effective transport clock in Hz, little-endian; zero unless initialized |
| 9 | 4 | Block count | Number of 512-byte sectors, little-endian; zero unless capacity is valid |
| 13 | 16 | SD CID | Raw card identification register; zero unless CID is valid |

When presence-known is clear, the card's presence is unknown; do not treat it as absent. When no card-detect input is configured, successful initialization establishes presence. CID bytes decode as manufacturer ID (byte 0), OEM ID (1-2, ASCII), product name (3-7, ASCII), product revision (byte 8, high nibble major and low nibble minor), serial number (9-12, big-endian), and manufacturing date (13-14: year offset from 2000 in the upper 8 bits and month in the lower 4 bits). Byte 15 contains the CID CRC/end bits. The numeric manufacturer ID is reported without a vendor-name lookup.

If initialized is set, configured, presence-known, present, CID-valid, and
capacity-valid must also be set. The report supports `GET_REPORT` when the card
reservation is available.

CID and capacity retained internally for SD diagnostics after initialization or I/O failure are not published in
Input Report 3 while the card is uninitialized. Its CID-valid and capacity-valid flags remain clear, and CID, clock,
and capacity fields remain zero, so a card failure does not invalidate the dashboard's live status stream.

## Feature Report 4

Feature report 4 accepts a 2-byte command payload: version `2`, then desired LED state (`0` off or `1` on). Setting a state is idempotent; the command is ignored if malformed or if the selected board has no controllable LED. The resulting state is reported in subsequent Input Report 2 telemetry.

## Report ID 5

Feature report 5 controls USB Mass Storage Class (MSC) ownership. Its SET payload is 2 bytes: version `2`, then desired state (`0` disabled or `1` enabled). Input report 5 carries a 4-byte status payload every 500 ms: version `2`, enabled, transition-pending, and request-rejected; the last three fields are booleans.

MSC is disabled at boot. Enabling requires an initialized card, an unmounted firmware filesystem, and no outstanding CLI storage command. Both enable and disable requests are rejected while storage work or its replies are pending. Use the [CDC CLI](cdc-cli.md) to explicitly unmount before handoff. The device briefly disconnects and re-enumerates with a CDC/HID/MSC configuration; its VID/PID, product string, and HID interface number remain unchanged. HID is unavailable during that short USB transition. WebHID reconnects the same authorized board automatically; other HID hosts should reopen the same device after re-enumeration. While the host owns the card, firmware block I/O returns not-ready. To return ownership, safely eject the drive on the host, then disable MSC over HID; a disable request is rejected until the host ejects it and allows removal. The device re-enumerates without MSC and firmware regains card ownership. Do not unplug the card or reset the device while the host is writing.

For a card invalidated by an I/O failure, the explicit
[CDC fault-recovery workflow](cdc-cli.md#faulted-msc-recovery) can detach faulted
MSC without claiming clean eject or sync. It uses the existing pending/rejected
status fields; normal HID enable/disable semantics are unchanged.

## Report ID 6

Feature Report 6 starts or cancels the [SD file self-test](sd-self-test.md).
Its payload is exactly 6 bytes: version `2` (byte 0), action (byte 1: `1` start,
`0` cancel), and little-endian size in MiB (bytes 2..5). Start accepts 1..128;
Cancel requires size zero. Firmware rejects an unavailable card, USB ownership,
an unmounted filesystem, invalid size, pending storage work, or an unavailable
card reservation. An existing mounted filesystem is required.

Input Report 6 publishes this 32-byte little-endian snapshot every 500 ms;
GET_REPORT with type Input can also retrieve it:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Version (`2`) |
| 1 | 1 | Sequence |
| 2 | 1 | State |
| 3 | 1 | Status flags: bit 0 test file remains, bit 1 volume remains mounted (normal for mounted-only tests) |
| 4 | 4 | Target bytes |
| 8 | 4 | Written bytes |
| 12 | 4 | Read/verified bytes |
| 16 | 4 | Write elapsed milliseconds |
| 20 | 4 | Read/verify elapsed milliseconds |
| 24 | 4 | Mismatched byte count |
| 28 | 1 | Execution error |
| 29 | 1 | FatFs FRESULT |
| 30 | 1 | Most recent command rejection (zero for none) |
| 31 | 1 | Reserved zero |

States are idle `0`, queued `1`, mounting `2`, writing `3`, reading/verifying `4`,
cleanup `5`, passed `6`, failed `7`, cancelled `8`. Errors are none `0`, busy `1`,
USB owner `2`, mounted filesystem `3`, size `4`, card unavailable `5`, space `6`,
existing test file `7`, filesystem I/O `8`, verification `9`, cleanup `10`,
filesystem not mounted `11`. Error `3` is retained for compatibility but is no
longer emitted. Starting requires an already-mounted filesystem; tests never
mount or unmount it.
Completion is published only after cleanup attempts. A command rejection does
not replace an ongoing test or its retained result. Hosts compute averages from
byte counters and durations; measurement boundaries and safety rules live in
[the self-test guide](sd-self-test.md).

## Host Implementations

- Python HIDAPI client and loopback Flask dashboard: `host/python/`.
- .NET 10 HidSharp monitor and loopback dashboard/API: `host/dotnet/`.
- Direct browser WebHID dashboard: `host/webhid/`, sharing the display code from `host/web/`.

Firmware cross-builds and hardware-free host tests do not verify physical host enumeration. Test the composite interface and CDC/HID behavior on RP2040 and RP2350 boards before relying on it in a product.

The 2026-10-15 static review follow-up fixes cached MSC transfer sense at handled
BOT and bus resets. Data paths, clocks and buffer defaults are unchanged; no
new throughput result is claimed. See [SD review scope and validation](sdcard-design.md#static-sd-review-follow-up-2026-10-15)
for reset ordering, host-model limits, existing overlap and pending HIL comparisons.
