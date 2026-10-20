# PicoForge USB Status Protocol

## Device Interfaces

The development device uses VID/PID `0xCAFE:0x4011` in both configurations. Replace this unallocated development identity before product release. The HID interface remains interface 2; when MSC is enabled, its interface is appended as interface 3.

- CDC ACM is the interactive [Embedded CLI console](cdc-cli.md); the prompt appears after the host asserts DTR. UART debug logs remain separate.
- HID uses vendor usage page `0xFF00`, usage `0x01`.
- USB product string: `PicoForge CDC + HID`.
- USB serial string: the Pico SDK unique board ID.

The HID report ID is the first byte of each report delivered by host APIs. Multi-byte integer fields are little-endian. Feature-report APIs may prepend an additional zero report-number byte on some Linux transports; host decoders accept that transport prefix and zero-filled report padding. Hosts read Feature Report 1 once when connecting for stable identity, then use periodic Input Reports 2 and 3 for changing values.

## Feature Report 1

Feature report 1 has a 42-byte payload, excluding the report ID. Its layout version is `3`.
The firmware release version uses the PicoUart build-time policy. Set the CMake cache variable
`PF_FIRMWARE_VERSION` or pass `--firmware-version 0.1.0` to
[`build.sh`](../tools/firmware/build.sh) or [`load.sh`](../tools/firmware/load.sh).
Both scripts also accept the `PF_FIRMWARE_VERSION` environment variable.
The default is `0.0.0-dev`; versions must be `MAJOR.MINOR.PATCH`, optionally followed by `-dev` or `-rcN`
(positive N without leading zeros). Each component is 0-255 without leading zeros.
[`version.cmake`](../firmware/version.cmake) validates the version before compilation.
The complete string is embedded in Pico SDK program metadata. HID reports the three numeric components only,
so `1.2.3-rc1` appears as `1.2.3` in the dashboard, matching PicoUart.
USB `bcdDevice` encodes major/minor as BCD (`1.2.3` becomes `0x0102`); major/minor above 99 produce a
configuration warning and `0x0000`, while HID retains the numeric version.
It is separate from the HID layout version. Runtime report 2 uses layout `1`; MSC status report 5 uses layout `3`;
other reports use their own documented layouts.
Updated hosts also accept identity layout `2` with four zero reserved bytes and show
firmware version as `Unknown`. Older hosts reject identity layout `3`: restart Python/.NET hosts with the updated
code and reload browser pages when upgrading firmware.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `3` |
| 1 | 1 | MCU ID | `1` = RP2040, `2` = RP2350 |
| 2 | 20 | Product name | NUL-padded ASCII, currently `PicoForge` |
| 22 | 16 | USB serial | ASCII hexadecimal unique board ID; same value as the USB serial string |
| 38 | 1 | Firmware major | Unsigned release component |
| 39 | 1 | Firmware minor | Unsigned release component |
| 40 | 1 | Firmware patch | Unsigned release component |
| 41 | 1 | Reserved | Zero |

All identity fields are stable for a connection. `clk_sys` is intentionally excluded because firmware may change it at runtime.

## Input Report 2

Input report 2 has a 25-byte payload and is published every 250 ms. Its initial layout version is `1`; firmware and
host implementations use this contract without legacy-layout handling.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `1` |
| 1 | 1 | Sequence | Increments for each accepted periodic report |
| 2 | 4 | Uptime | Unsigned milliseconds, little-endian; wraps after `2^32` ms |
| 6 | 4 | `clk_sys` | Current unsigned frequency in Hz, little-endian |
| 10 | 1 | Flags | Bit 0: USB mounted; bit 1: CDC DTR/open; bit 2: temperature valid; bit 3: board LED available; bit 4: board LED on; other bits reserved |
| 11 | 2 | Temperature | Signed centidegrees Celsius, little-endian; meaningful only when bit 2 is set |
| 13 | 2 | Core 0 CPU usage | Unsigned basis points (0..10000); `65535` means not yet available |
| 15 | 2 | Core 1 CPU usage | Unsigned basis points (0..10000); `65535` means not yet available |
| 17 | 4 | RAM reserved | Static on-chip SRAM reservations in bytes |
| 21 | 4 | Total RAM | SDK-selected on-chip SRAM capacity in bytes |

The sequence byte wraps modulo 256. Temperature comes from the MCU's internal sensor and is an estimate, not a calibrated ambient measurement. If bit 4 is set, bit 3 must also be set. The input report supports `GET_REPORT`, returning current telemetry. The current system clock is sampled for each report, so hosts should treat it as dynamic.

CPU usage is measured independently per core over the latest interval of at least 250 ms, as one minus that core's
FreeRTOS idle-task runtime divided by elapsed SDK 64-bit microsecond time. One busy core and one idle core report
100% and 0%, respectively. Adjacent wall-clock/idle sampling skew is saturated at 0% rather than wrapping. The first
sample establishes a baseline and reports unavailable; shorter requests reuse the previous result rather than
resetting the interval. Interrupt time is attributed to the interrupted task by FreeRTOS, so this is task-load
telemetry, not separate ISR accounting. Waiting tasks contribute idle time only when the idle task runs; polling
loops count as busy.

RAM reserved is main SRAM through the SDK linker `__end__`, plus scratch-bank data and both reserved core stacks.
Statically allocated FreeRTOS task stacks and control blocks are already included in main SRAM and are not counted
twice. Total capacity comes from the SDK's `SRAM_BASE`/`SRAM_END`, not a board-specific constant. It excludes external
RAM and does not measure stack high-water marks or live libc heap allocations. FreeRTOS dynamic allocation remains
disabled. This value is normally constant for a firmware image.

Both dashboards display Core 0 and Core 1 CPU usage separately, and show reserved RAM and total on-chip SRAM as
separately labeled values in Runtime status. System clock
appears once in the identity row, still updated from this runtime report. Unavailable fields display `--`.
Report sequence remains available in the protocol and host decoders for diagnostics, but is not displayed.
The Last report timestamp indicates dashboard freshness.

## Input Report 3

Input report 3 has a 29-byte payload and is normally published every 1000 ms.
Its layout version is `2`. Dashboards label the card-detect state **SD presence** in Runtime status; they do not
Its layout version is `2`. Dashboards show SD transport and initialization in the SD Card section; they do not
display a memory-type field because SD is the only supported storage device. The shared dashboards hide the SD Card
and Memory Tests sections only when presence is known and absent; unknown presence keeps both sections visible. The
component-type byte remains in the wire layout for compatibility. With the reference board profile, presence comes from
the active-high DET input on GP8, independently of whether card initialization or filesystem mounting succeeds.
The storage worker initializes and mounts detected
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

When presence-known is clear, the card's presence is unknown; do not treat it as absent. When no card-detect input is configured, successful initialization establishes presence. CID bytes decode as manufacturer ID (byte 0), OEM ID (1-2, ASCII), product name (3-7, ASCII), product revision (byte 8, high nibble major and low nibble minor), serial number (9-12, big-endian), and manufacturing date (13-14: year offset from 2000 in the upper 8 bits and month in the lower 4 bits). Byte 15 contains the CID CRC/end bits. Host dashboards show names for recognized manufacturer IDs and retain the numeric ID; unrecognized IDs are labeled unknown rather than guessed.

If initialized is set, configured, presence-known, present, CID-valid, and
capacity-valid must also be set. The report supports `GET_REPORT` when the card
reservation is available.

CID and capacity retained internally for SD diagnostics after initialization or I/O failure are not published in
Input Report 3 while the card is uninitialized. Its CID-valid and capacity-valid flags remain clear, and CID, clock,
and capacity fields remain zero, so a card failure does not invalidate the dashboard's live status stream.

## Input Report 7

Input report 7 has a 3-byte payload, excluding the report ID, with no padding. Its layout version is `2`. It is
push-only: firmware publishes it every 1000 ms while the HID endpoint is ready, and `GET_REPORT` is unsupported (it
returns no data). The sequence increments only for an accepted report and wraps modulo 256.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Version (`2`) |
| 1 | 1 | Sequence |
| 2 | 1 | Filesystem format |

| Value | Format |
| --- | --- |
| 0 | unknown |
| 1 | FAT12 |
| 2 | FAT16 |
| 3 | FAT32 |
| 4 | exFAT |

The format is that of the last successfully mounted filesystem, cached when FatFs mount succeeds; it is
not live mounted state. It reverts to unknown when a mount or format starts, after a successful unmount, on card
removal, when an uninitialized card forces unmount, and when MSC ownership is acquired. A failed unmount leaves the
cached value in place. No HID report carries live mounted state; use the [CDC CLI](cdc-cli.md) filesystem commands.

## Feature Report 4

Feature report 4 accepts a 2-byte command payload: version `2`, then desired LED state (`0` off or `1` on). Setting a state is idempotent; the command is ignored if malformed or if the selected board has no controllable LED. The resulting state is reported in subsequent Input Report 2 telemetry.

## Report ID 5

Feature report 5 controls USB Mass Storage Class (MSC) ownership. Its safe-toggle SET payload remains 2 bytes:
version `2`, then desired state (`0` disabled or `1` enabled). The distinct confirmed force-return command is exactly
`[3, 2]`, excluding the report ID; layout `2` never interprets action `2` as force.
Input report 5 carries a 4-byte status payload every 500 ms: version `3`, enabled, transition-pending, and
request-rejected; the last three fields are booleans. Updated hosts accept legacy status layout `2` but only enable
request-rejected; the last three fields are booleans. Updated hosts accept legacy MSC status layout `2` but only enable
force return after observing MSC status layout `3`. Older host software must be updated before using this firmware.

MSC is disabled at boot. Enabling requires initialized SD media and no outstanding storage command
or replies. If firmware has mounted the filesystem, enabling queues an unmount on the sole storage worker and keeps
the transition pending until it completes. Successful unmount pauses automount before USB ownership acquisition;
failed unmount rejects the request and leaves firmware ownership intact. No formatting occurs.
Both enable and disable requests are rejected while unrelated storage work or its replies are pending.
The device briefly disconnects and re-enumerates with a CDC/HID/MSC configuration; its VID/PID, product string,
and HID interface number remain unchanged. HID is unavailable during that short USB transition. WebHID reconnects
the same authorized board automatically; other HID hosts should reopen the same device after re-enumeration.
While the host owns the card, firmware block I/O returns not-ready. The legacy layout-2 disable request requires
host eject and permission to remove media. The confirmed dashboard Disable flow below does not require host eject.
The device re-enumerates without MSC and firmware regains storage ownership. Do not unplug the card or reset
the device while the host is writing.

The layout-2 safe-disable command and `POST /api/msc` with `{"enabled":false}` remain available for clients that
manually eject first. Host apps do not invoke OS unmount/eject tools. The dashboard Disable button uses the
confirmed Pico-side return below instead of the legacy safe-disable command.

### Confirmed force return

The **Disable USB drive** dashboard action warns about data loss and requires explicit confirmation.
Python and .NET expose `POST /api/msc/force-return` with `{"confirm_data_loss":true}`; absent, false or non-boolean
confirmation is rejected before HID delivery. WebHID sends the same `[3, 2]` command directly.
There is no separate Force button. On Windows, Linux and macOS the host only delivers the HID command; Pico
performs detach, ownership transfer and remount. No host OS eject/unmount command or Linux-specific permission
is required. Older firmware without MSC status layout `3` cannot use dashboard Disable and displays an update message.
This command never synthesizes a successful SCSI eject.

Firmware still rejects busy storage or a failed device-side sync. After a successful sync, it disconnects USB
before releasing SD ownership under the shared media reservation. The sole storage worker attempts
initialization and mounting once. Pending remains set until that attempt completes. A failed initialization/mount
sets request-rejected with enabled false, logs the failure, and leaves storage unmounted with automatic retries
blocked until an explicit successful `fs mount`. A pre-detach failure leaves MSC enabled. No automatic formatting,
filesystem repair, or replay of failed writes is performed.

**Device sync cannot flush writes cached in the host.** Forced return can lose data or corrupt the filesystem even
when Pico remounts successfully. Stop host file activity first and manually safely eject whenever possible before Disable.
USB reconnection does not itself prove the remount succeeded; wait for pending to clear and inspect rejection/logs.
This behavior has host-runnable regression coverage, including identical command delivery for Windows/Linux/macOS
platform identities; physical validation on those operating systems remains outstanding.
Live forced-detach validation requires separate authorization. WebHID also requires a browser supporting WebHID;
OS-independent firmware behavior does not imply all browsers (for example Safari) support that API.

For a card invalidated by an I/O failure, the explicit
[CDC fault-recovery workflow](cdc-cli.md#faulted-msc-recovery) can detach faulted
MSC without claiming clean eject or sync. It uses the existing pending/rejected
status fields; normal HID enable/disable semantics are unchanged.

## Report ID 6

Feature Report 6 starts or cancels the [SD file self-test](sd-self-test.md).
Its payload is exactly 6 bytes: version `2` (byte 0), action (byte 1: `1` start,
`0` cancel), and little-endian size in MiB (bytes 2..5). Start accepts 1..1024;
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

## Feature Reports 8 and 9

Feature report 8 reads or saves persisted boot settings. Its payload is the five-byte, little-endian layout documented
in [persistent settings](settings.md#host-dashboard-settings). The host reads the report back and compares all fields
before continuing.

Feature report 9 accepts the two-byte payload `[1, 1]` (layout version 1, reboot action), excluding the report ID.
Firmware rejects malformed commands and requests while MSC or storage work is active. An accepted request is handled
by the USB task after the HID control transfer returns; firmware waits one second before watchdog reset to allow the
host to receive the acknowledgement. The Save & Reboot action sends report 9 only after report 8 readback matches.

## Host Implementations

- Python HIDAPI client and loopback Flask dashboard: `host/python/`.
- .NET 10 HidSharp monitor and loopback dashboard/API: `host/dotnet/`.
- Direct browser WebHID dashboard: `host/webhid/`, sharing the display code from `host/web/`.

Firmware cross-builds and hardware-free host tests do not verify physical host enumeration. Test the composite interface and CDC/HID behavior on RP2040 and RP2350 boards before relying on it in a product.

The [2026-10-15 static review record](tests/records/2026-10-15-static-sd-review-followup.md) covers the MSC reset-sense fix and its validation limits.
