# USB HID Report Reference

This document is the authoritative HID wire contract. For CDC CLI behavior, see the
[USB CDC CLI guide](../../guides/usb-cdc-cli.md).

## Report ID Allocation

Report ID `0` is unused. Unassigned IDs have no payload and require no padding.

| IDs | Generic role | PicoForge assignments |
| --- | --- | --- |
| `1`-`9` | Device information and telemetry | `1` Identity; `2` Board Configuration; `3` Telemetry. |
| `10`-`19` | Application status | `10` Storage Status; `11` Self-Test Status. |
| `20`-`29` | Device and application control | `20` Control Command & Result. |
| `30`-`39` | Application diagnostics | Unassigned. |
| `40`-`255` | Expansion | Unassigned. |

Report ranges group responsibilities; the descriptor and firmware define each report's type, length, and permitted
operations.

PicoForge exposes CDC ACM, a vendor-defined HID interface, and optional MSC while the host owns the SD card. CDC ACM
uses control interface 0 and data interface 1; HID is interface 2 in both configurations. When enabled, MSC is
appended as interface 3, so toggling MSC does not change the HID interface number. The development VID/PID is
`0xCAFE:0x4011`, not a product identity. HID uses vendor usage page `0xFF00`, usage `0x01`, with no boot protocol.
The USB product string is `PicoForge CDC + HID`; its serial string is the Pico SDK unique board ID.

CDC ACM is the [Embedded CLI console](../../guides/usb-cdc-cli.md). UART debug logs use the board monitor UART, independently of
USB CDC. HID carries no CLI text, file data, or SD block data. MSC is the host-facing block interface while it owns
the card.

## Ownership

| Function | Report | Contract |
| --- | --- | --- |
| Identity, capabilities, firmware and memory information | Feature `1` | Read-only and stable for one boot/USB connection. |
| Persisted profiles and board configuration | Feature `2` | Profiles are next-boot settings. |
| Live board measurements | Input `3` | Periodic telemetry; unavailable readings use their sentinels or validity flag. |
| SD, filesystem, and MSC state | Input `10` | Storage snapshot with independent SD-sample validity. |
| Self-test progress/results | Input `11` | Requires Report `1` bit 5; sampled independently of storage. |
| LED, reboot, MSC, self-test, and settings operations | Feature `20` | Typed SET opcode; GET returns the latest operation result/data. |
| CLI and normal storage commands | CDC ACM | Text command interface, not HID payloads. |
| Host file/block access | MSC | Available only while the host owns the SD card. |

Read-only status never performs an action. GET never consumes a sample, repeats an operation, or clears its result.
Report `20` confirms command validation or acceptance; reports `10` and `11` report storage and self-test state,
respectively.

## Report IDs

These are logical groups, not separate HID usage pages or mandatory dashboard tabs.

| Report ID | Name | Type | Direction | Payload | Purpose |
| --- | --- | --- | --- | ---: | --- |
| `1` | Device Identity & Capabilities | Feature | Host reads | 26 bytes | Product identity, firmware, MCU, board model, capabilities, and SRAM accounting. |
| `2` | Board Configuration | Feature | Host reads | 17 bytes | Stored clock and SD-card profiles, reset cause, and configured GPIOs. |
| `3` | Board Telemetry | Input | Device publishes / host reads | 22 bytes | PicoUART-compatible board measurements plus PicoForge status. |
| `10` | Storage Status | Input | Device publishes / host reads | 34 bytes | SD, filesystem, and MSC status. |
| `11` | Self-Test Status | Input | Device publishes / host reads | 32 bytes | Self-test state and progress. |
| `20` | Control Command & Result | Feature | Host writes/reads | 9 bytes | LED, reboot, MSC, self-test, and settings operations. |

Names are documentation labels, not additional wire fields.

**Wire rules:** On the USB HID wire, each report begins with its nonzero report ID. In the tables below, offset zero
is the first payload byte after that ID; payload sizes exclude the ID. Host APIs expose the ID differently: HIDAPI
buffers include the report-ID byte at index zero, while WebHID supplies the report ID separately and exposes only the
payload in its data buffer. A .NET or other host adapter must follow its library's documented convention and
normalize to the payload defined here before decoding. Some Feature-report APIs add a transport framing byte or
fixed-size padding; remove only framing explicitly documented by that API, never a payload byte based only on its
value. Validate the exact payload length after normalization. Multi-byte integers are little-endian; signed integers
use two's complement. All reports fit within 64 bytes including report ID. The layouts use version `1`, independent
of firmware semantic version.

### Connection and Read Flow

After opening HID, read Feature `1` first. Use its version and product ID to select the protocol, then validate the
full report length and fields before decoding product-specific data or enabling commands. A PicoForge host must not
decode this report as PicoUART merely because Report `3` shares a field prefix: PicoForge Report `3` is longer, and
exact-length PicoUART decoders are not compatible with it. Read Feature `2` for configuration, then subscribe to
Input reports `3` and `10`, and subscribe to Report `11` only when Report `1` capability bit 5 is set. Submit
operations through Feature `20`; its result acknowledges validation or acceptance, while the corresponding Input
report owns asynchronous operation state.

## Report ID 1: Device Identity & Capabilities

Read after connection. Identity and capabilities remain stable for that boot.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Layout version, `1`. |
| 1 | 2 | `product_id` | Product-family ID, little-endian; `0x0002` is PicoForge. |
| 3 | 8 | `device_uid` | Opaque SDK board identity bytes in USB serial-string order. |
| 11 | 1 | `firmware_major` | Firmware semantic-version major. |
| 12 | 1 | `firmware_minor` | Firmware semantic-version minor. |
| 13 | 1 | `firmware_patch` | Firmware semantic-version patch. |
| 14 | 1 | `mcu` | `1` RP2040; `2` RP2350. |
| 15 | 1 | `board_model` | Configured target: `1` Pico; `2` Pico 2; `3` Pico 2 W; `4` Pico W. |
| 16 | 1 | Reserved | Zero; hosts must ignore this byte. |
| 17 | 1 | `capabilities` | Static device and storage-transport capabilities. |
| 18 | 4 | `ram_total_bytes` | Total on-chip SRAM for the MCU/linker layout; not free memory. |
| 22 | 4 | `ram_reserved_bytes` | Static and SDK-reserved SRAM for this firmware build. |
| **Total** | **26** | | **27 bytes including report ID.** |

The discovery prefix is three bytes: version at offset 0 and product ID at offsets 1-2. Read the descriptor-declared
Feature length and validate the prefix and exact payload before decoding fields or enabling commands. Display the
8-byte `device_uid` as **Serial / UID**: two uppercase hex characters per byte in order, matching the USB serial
string. It is the Pico SDK board ID, not an integer or a host-generated identifier. Preserve unknown MCU and
board-model values for diagnostics; do not guess their meaning. For assigned board models, `mcu` must match the
board table below: model `1` requires MCU `1`; models `2` and `3` require MCU `2`. Model `0` makes no board claim
and reports the MCU independently. A mismatch is an invalid identity report, not a reason to infer or correct a value.

### Product IDs and Capabilities

Product names are selected by product-family ID; retain a readable USB product descriptor for operating-system
device lists.

| Product ID | Meaning |
| ---------: | ------- |
| `0x0000` | Reserved / unspecified. |
| `0x0001` | PicoUart, across supported board/MCU variants. |
| `0x0002` | PicoForge, across supported board/MCU variants. |
| `0x0003`-`0xfffe` | Available for separately defined product families. |
| `0xffff` | Reserved. |

Product IDs identify families, not firmware releases, MCU types, individual devices, or compatibility. The discovery
prefix is version at offset 0 and little-endian product ID at offsets 1-2. Read the descriptor-declared Feature length
and require at least that three-byte prefix before inspecting it. The prefix alone does not validate the full report:
select the product and layout, then validate the exact length and all fields before enabling commands. A known product
with an unsupported layout version is an explicit unsupported-layout error. Display unknown IDs as
`Unknown product (0xNNNN)` without enabling product-specific commands.

Capability bits:

| Bit | Meaning |
| ---: | --- |
| 0 | Board identification LED available. |
| 1 | Confirmed standalone reboot available. |
| 2 | Persistent boot-configuration save available. |
| 3 | SPI storage transport selected for this firmware build. |
| 4 | SDIO-PIO storage transport selected for this firmware build. |
| 5 | SD self-test control and status available. |
| 6 | Explicitly confirmed settings save/reboot while USB owns storage. |
| 7 | Reserved; zero. |

For bit 1, the host must explicitly confirm the standalone reboot request. Bit 2 advertises saving persistent boot
settings with a firmware-scheduled reboot; bit 6 adds the forced host-owned save path and does not replace bit 2.
Clients must not send a forced save without bit 6 or silently fall back to normal save/reboot. Older clients that
reject the previously reserved bit require an update alongside this firmware. Exactly one of bits 3 and 4 is set, identifying the transport
selected at build time. Either selected storage transport implies that MSC control is supported; MSC has no separate
capability bit. Use the transport bit to interpret report `2`'s storage GPIO layout. Bit 5 makes self-test control
and status available together; when clear, Report `11` is unavailable and must not be published, and opcode `4`
returns Unsupported (`4`). Bits 0, 1, 2, and 5 respectively gate opcodes `1`, `2`, `6`, and `4`. Opcode `3` is
supported whenever bit 3 or 4 selects a storage transport. Keep firmware version for diagnostics; do not infer
capabilities from the version.

### Board Models

| Board model | Build target | MCU | Meaning |
| ----------: | ------------ | --- | ------- |
| `0` | Unknown / unspecified | Reported separately | No specific board identity claimed. |
| `1` | Pico | RP2040 | Configured Pico target. |
| `2` | Pico 2 | RP2350 | Configured Pico 2 target. |
| `3` | Pico 2 W (`pico2_w`) | RP2350 | Configured Pico 2 W target; UART0 uses GP0/GP1, matching Pico. |
| `4` | Pico W (`pico_w`) | RP2040 | Configured Pico W target; UART0 uses GP0/GP1, matching Pico. |
| `5`-`255` | Unassigned | - | Reserved. |

Board model is the configured build target, not automatic hardware detection.

### Firmware RAM Reservation

Display `ram_reserved_bytes` against `ram_total_bytes` from this report as **Firmware RAM reserved / total**, in KiB
and optionally as a percentage. Reject zero total or a reservation greater than total. This is build accounting, not
runtime memory pressure, free heap, or stack high-water use.

The reservation includes linked SRAM from `SRAM_BASE` through `__end__`, scratch X/Y linked sections, and both SDK
core-stack reservations. It excludes remaining heap capacity and does not measure live allocations or stack use.
Total SRAM is `SRAM_END - SRAM_BASE`, including scratch banks. Use the firmware's authoritative accounting and count
each reservation once.

## Report ID 2: Board Configuration

Read-only; reject SET. Read after connection and again after reboot or re-enumeration. Fields describe the configured
board and persisted clock and SD-card profiles selected for the next boot. Report `1` identifies the build-selected
storage transport.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Layout version, `1`. |
| 1 | 2 | `stored_sysclk_profile` | Stored system-clock setting in MHz, little-endian: `0` build clock, `150`, `200`, or `250`. |
| 3 | 1 | `stored_sdcard_profile` | `0` Standard; `1` High-Speed. |
| 4 | 1 | `monitor_uart_index` | Hardware UART index; `255` if unavailable. |
| 5 | 1 | `monitor_uart_tx_gpio` | TX GPIO number; `255` if unassigned. |
| 6 | 1 | `monitor_uart_rx_gpio` | RX GPIO number; `255` if unassigned. |
| 7 | 6 | `storage_gpios` | SDIO/SPI GPIO numbers, selected-transport order below. |
| 13 | 1 | `board_flags` | Bit 0 card detect configured; bit 1 LED configured; bit 2 monitor UART enabled; other bits zero. |
| 14 | 1 | `reset_cause` | Cause of the reset that began this boot. |
| 15 | 1 | `led_gpio` | LED GPIO number; `255` if unavailable. |
| 16 | 1 | `card_detect_gpio` | Card-detect GPIO number; `255` if not configured. |
| **Total** | **17** | | **18 bytes including report ID.** |

The stored clock and SD-card profile values are the persisted settings for the next boot, not measured runtime
values; report `3` carries the active system-clock frequency and report `10` carries the active SD speed mode and bus
clock. After a successful opcode `6` save, firmware returns the verified saved values in the control result and
queues a reboot through the existing deferred USB reboot path. During the one-second grace period, firmware
continues servicing USB control transfers so the host can read the completion result before reset.
Hosts must not send a second reboot command or
require a configuration read before disconnect; reread report `2` after reconnection. The SD-card profile is a request applied to the
build-selected transport from report `1`: Standard disables High-Speed timing; High-Speed permits CMD6 negotiation
within the transport's firmware ceiling, system-clock limit, and card capability. It does not guarantee that
High-Speed timing was negotiated. GPIO values are GPIO numbers, not header-pin numbers; `255` means unassigned. For
SDIO-PIO, storage slots 0-5 are CLK, CMD, DAT0-DAT3. For SPI, slots 0-3 are CLK, MOSI, MISO, CS and slots 4-5 are
`255`. Report `1`'s transport capability determines the slot interpretation. These pins describe the build profile;
they do not prove a peripheral is active.

Configuration validity rules: reserved `board_flags` bits must be zero. The card-detect flag is set exactly when
`card_detect_gpio` is assigned; otherwise that field is `255`. The LED-configured flag is set exactly when
`led_gpio` is assigned, and Report `1` bit 0 agrees with whether that configured LED is available for identification
control. `monitor_uart_index` is `0` or `1` when available and `255` when unavailable; TX and RX are either both
assigned or both `255`. If the monitor-UART-enabled flag is set, the index and both GPIOs must be assigned; when
clear, those pins may still describe a configured but disabled UART. Every assigned GPIO must be valid for the
reported MCU and configured board. For SDIO-PIO all six storage GPIOs must be assigned; for SPI the first four must
be assigned and slots 4-5 must be `255`. GPIO values are numbers, not pin-header positions.

The profile is the transport-neutral replacement for the current per-transport MHz setting. During migration,
SDIO_PIO settings `0`/`25` map to Standard and `50` maps to High-Speed; SPI `25` maps to Standard and `50` maps to
High-Speed. SPI `0` maps to High-Speed to preserve its current 50 MHz board-profile ceiling. With no valid stored
record, use Standard for SDIO_PIO and High-Speed for SPI to preserve current defaults. A new-format save stores the
profile enum; Standard maps to the selected transport's 25 MHz setting and High-Speed to its 50 MHz setting.

Reset cause is independent of clock-selection reason and may be Unknown if the platform cannot distinguish it.
PicoForge currently reports Watchdog (`2`) when the SDK detects a watchdog reset, including software-requested
reboots through `watchdog_reboot()`. Otherwise it reports Unknown (`0`); it does not infer power-on, software-only,
or HardFault causes without distinguishing evidence.
Values: `0` Unknown; `1` Power-on when reliably identified; `2` watchdog without established software-requested cause;
`3` software-requested when supported by evidence; `4` watchdog recovery after a recorded HardFault; `5`-`255`
unassigned. Never infer Power-on or software-requested reset from missing watchdog evidence.

## Report ID 3: Board Telemetry

Input report published nominally every 250 ms. GET may return a current snapshot if supported. The first 20 payload
bytes match PicoUART telemetry layout version 1 exactly; PicoForge-specific fields follow so shared fields retain
their offsets. Sequence increments for each accepted Input publication and wraps modulo 256; a failed/busy
publication does not consume a sequence.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Telemetry layout version, `1`. |
| 1 | 1 | `led_flags` | Bit 0: identification output on; bits 1-7 zero. |
| 2 | 2 | `temperature_centidegrees_celsius` | Signed RP2 internal temperature estimate in hundredths of a degree Celsius. |
| 4 | 8 | `uptime_ms` | SDK monotonic milliseconds since boot. |
| 12 | 2 | `core0_service_load_basis_points` | Core 0 service load; 10000 = 100%; `65535` unknown. |
| 14 | 2 | `core1_service_load_basis_points` | Core 1 service load; same scale and sentinel. |
| 16 | 4 | `system_clock_hz` | Current SDK-reported `clk_sys` frequency; nonzero. |
| 20 | 1 | `sequence` | PicoForge extension: accepted publication sequence, modulo 256. |
| 21 | 1 | `picoforge_flags` | PicoForge extension: USB, CDC, sensor-valid, and LED-available flags below. |
| **Total** | **22** | | **23 bytes including report ID.** |

`picoforge_flags`: bit 0 USB mounted; bit 1 CDC DTR/open; bit 2 temperature valid; bit 3 identification output
available; bits 4-7 zero. Bit 0 means the USB device is configured with the host; it does not indicate MSC ownership.
When temperature-valid is clear, the temperature field is zero and must not be displayed as a measurement.
`led_flags` bit 0 is set only when the identification output is available and on. The PicoUART-compatible
`led_flags` definition remains unchanged; availability is reported by the PicoForge extension.

Per-core load is one minus FreeRTOS idle runtime over a window of at least 250 ms. The first sample establishes a
baseline and is unavailable. Shorter requests reuse the last completed window. This measures task load; interrupt
time is charged to the interrupted task, and polling loops count as busy. It is not calibrated remaining capacity.
Valid load values are `0`-`10000` basis points inclusive; `65535` means unavailable. Values `10001`-`65534` are
invalid and hosts must reject the report value rather than clamp it. Unavailable values are not zero measurements.
Hosts show them as `--`, keep report freshness separate from uptime, and display the reported running clock rather
than substituting a requested setting. Temperature is an estimated die temperature, not calibrated ambient
temperature.

## Report ID 10: Storage Status

This Input report covers card, filesystem, and MSC state. Publish nominally every 500 ms while HID is ready. GET
returns a non-consuming current snapshot. The sequence increments for each accepted publication and wraps modulo 256.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Storage-status layout version, `1`. |
| 1 | 1 | `sequence` | Accepted report sequence. |
| 2 | 1 | `sd_flags` | SD state and sample-valid flags below. |
| 3 | 1 | `component_type` | `0` unavailable; `1` SD card. |
| 4 | 1 | `bus_mode` | `0` unknown; `1` SDIO-PIO; `2` SPI. |
| 5 | 4 | `bus_clock_hz` | Effective transport clock; zero unless the SD sample is valid and initialized. |
| 9 | 1 | `speed_mode` | `0` unknown; `1` Standard timing; `2` High-Speed timing actually selected. |
| 10 | 4 | `block_count` | 512-byte sectors; zero unless capacity is valid. |
| 14 | 16 | `cid` | Raw SD CID; zero unless valid. |
| 30 | 1 | `filesystem_format` | Last successfully mounted format; enum below. |
| 31 | 1 | `msc_enabled` | Boolean: MSC owns storage. |
| 32 | 1 | `msc_transition_pending` | Boolean: ownership transition/remount is active. |
| 33 | 1 | `msc_request_rejected` | Boolean: last MSC request/transition was rejected or failed. |
| **Total** | **34** | | **35 bytes including report ID.** |

The three MSC fields describe the ownership transition, not SD sample validity. `msc_enabled` is `1` only when the
host owns the card and the MSC configuration is active. `msc_transition_pending` is `1` from acceptance of a valid
MSC request that requires a transition until ownership and the resulting USB/storage state settle; it remains `0`
for an already-in-state no-op. HID may be temporarily unavailable while USB disconnects and re-enumerates.
`msc_request_rejected` describes the most recent syntactically valid opcode `3` request: clear it when that request
is received, then set it if the request is rejected or its transition fails. Malformed report envelopes do not
change these fields.

All multi-byte fields are little-endian. SD flags: bit 0 storage configured; bit 1 presence known; bit 2 present;
bit 3 initialized; bit 4 CID valid; bit 5 capacity valid; bit 6 current SD sample valid; bit 7 zero. If
bit 6 is clear, presence and card values must not be presented as current; component type, bus mode, clock, speed mode,
block count, and CID are zero. If bit 6 is set and initialized is set, bits 0-3 must all be set; bits 4 and 5
indicate whether the corresponding CID and capacity values are valid. If initialized is clear, speed mode is `0`.
Unknown presence is not absence. If no card-detect input is configured, successful initialization establishes
presence. A known-absent card has bit 1 set and bit 2 clear; it cannot be initialized and cannot have valid CID or
capacity. Initialization requires a configured, present card and therefore bits 0-3 set. Bits 4 or 5 cannot be set
unless initialized and bit 6 are set; when either validity bit is clear, its associated CID or block count is zero.
When bit 6 is clear, bits 1-5 are clear. When bit 6 is set, bit 0 must be set, `component_type` is `1`, and `bus_mode`
agrees with exactly one of Report `1`'s selected-transport capability bits. If bit 0 is clear, bits 1-6 and all
current card fields are zero. `msc_enabled`, `msc_transition_pending`, and `msc_request_rejected` are each exactly
`0` or `1`; they describe USB ownership/transition outcome independently of SD sample validity.

The device invalidates the SD sample as soon as an MSC ownership transition is accepted. While
`msc_transition_pending` or `msc_enabled` is `1`, `sd_flags` bit 6 is clear, bits 1-5 are clear, and all current
card fields (`component_type`, `bus_mode`, clocks, `speed_mode`, `block_count`, and `cid`) are zero. Bit 0 continues
to report whether storage is configured. After a transition settles with firmware ownership restored, publish a new
valid SD sample only after refreshing it; do not reuse the pre-transition sample as current. `filesystem_format`
remains a cached last-successful-mount value and follows its lifecycle rules below; it is not proof that a volume is
currently mounted.

`speed_mode` reports the active card timing mode, not the stored request: `1` means Standard/default-speed timing;
`2` means the card accepted and the host selected High-Speed timing. It may be `1` when the stored profile requests
High-Speed but the card or transport cannot use it. `bus_clock_hz` is the effective clock and must be nonzero for an
initialized, valid SD sample.

CID is the SD 16-byte register: manufacturer ID byte 0; ASCII OEM ID bytes 1-2; ASCII product name bytes 3-7;
revision nibbles byte 8 (high nibble major, low nibble minor); big-endian serial bytes 9-12; manufacturing date
bytes 13-14: byte 13's high nibble is reserved, the year offset from 2000 is byte 13's low nibble followed by byte
14's high nibble, and the month is byte 14's low nibble; CRC/end bits are byte 15. The raw bytes are authoritative;
decoded text is ASCII and may contain padding. Hosts may display recognized manufacturer names but must retain the
numeric manufacturer ID and label unknown IDs as unknown, not guess. The serial identifies the card and should only
be displayed or logged where the user needs it; do not use it as the device UID or transmit it to unrelated services.

Filesystem format values: `0` Unknown, `1` FAT12, `2` FAT16, `3` FAT32, `4` exFAT. This is the last successfully
mounted filesystem, not live mounted state. It returns to Unknown when mounting/formatting starts, after successful
unmount, on card removal, when an uninitialized card forces unmount, or when MSC ownership is acquired. A failed
unmount leaves the cached value unchanged. Use the [CDC CLI](../../guides/usb-cdc-cli.md) to query live mounted state.

## Report ID 11: Self-Test Status

This Input report carries self-test state and progress independently from storage status. Publish nominally every
500 ms while HID is ready. GET returns a non-consuming current snapshot. Its sequence increments for each accepted
publication and wraps modulo 256.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Self-test-status layout version, `1`. |
| 1 | 1 | `sequence` | Accepted report publication sequence. |
| 2 | 1 | `self_test_state` | State enum below. |
| 3 | 1 | `self_test_flags` | Bit 0 test file remains; bit 1 filesystem remains mounted; other bits zero. |
| 4 | 4 | `target_bytes` | Requested test size. |
| 8 | 4 | `written_bytes` | Bytes written. |
| 12 | 4 | `read_bytes` | Bytes read and verified. |
| 16 | 4 | `write_elapsed_ms` | Write-phase duration. |
| 20 | 4 | `read_elapsed_ms` | Read/verify-phase duration. |
| 24 | 4 | `mismatch_count` | Mismatched bytes. |
| 28 | 1 | `self_test_error` | Execution error enum. |
| 29 | 1 | `filesystem_result` | FatFs result when applicable. |
| 30 | 1 | `self_test_request_error` | Most recent self-test request error; zero none. |
| 31 | 1 | `reserved` | Zero. |
| **Total** | **32** | | **33 bytes including report ID.** |

Self-test states: `0` Idle, `1` Queued, `2` Preparing, `3` Writing, `4` Reading/verifying, `5` Cleanup, `6` Passed,
`7` Failed, `8` Cancelled; `9`-`255` unassigned. Error values: `0` none, `1` busy, `2` USB owns storage,
`3` reserved (never emitted), `4` invalid size, `5` card unavailable, `6` insufficient space, `7` test file exists,
`8` filesystem I/O, `9` verification, `10` cleanup, `11` filesystem not mounted; `12`-`255` unassigned.
`self_test_error` is zero except in Failed state, where it uses execution errors `5`, `6`, or `8`-`10`. Errors `1`,
`2`, `4`, `7`, and `11` are request/precondition rejections and appear only in `self_test_request_error`; code `3`
is reserved and never emitted. A card or space failure discovered during execution is error `5` or `6` in
`self_test_error`; the same condition detected before accepting a start is reported in `self_test_request_error`.
Filesystem I/O error `8` may appear in `self_test_request_error` only when the preflight check of the existing test
file fails before the test is accepted; after acceptance, filesystem I/O failures use `self_test_error`. Thus error
`8` in the two fields is distinguished by whether the test run was accepted. `self_test_request_error` may contain
`1`, `2`, `4`-`8`, or `11`; codes `9` and `10` are execution-only. It is set when a valid-envelope opcode `4`
request is rejected and cleared when a subsequent opcode `4` request is accepted, including an accepted cancel
no-op. A malformed report envelope does not change it. After reboot it is zero.
`filesystem_result` is nonzero and contains the FatFs result when `self_test_error` is `8` (filesystem I/O). For
error `10` (cleanup), it contains the FatFs result only when the cleanup failure came from a FatFs operation;
otherwise it is zero. It is zero for every other execution error and in all states other than Failed. Unknown enum
values must be retained and displayed as unknown, but treated as invalid for state transitions and safety decisions.

The self-test uses the [default-mounted filesystem](../architecture.md#storage-ownership-lifecycle); startup and
normal USB return trigger mounting independently of opcode `4`. The test never mounts or unmounts it.
A start request is accepted only after preflight checks succeed and storage is reserved; acceptance publishes Queued,
after which the operation
may advance through Preparing and execution. Before acceptance, size, ownership, card, space, existing-file, and
file-check errors reject the start: no test run is queued, and Report `11` retains the prior test result while
updating `self_test_request_error`. Error `11` reports that the required filesystem is not mounted; error `3` is
reserved to preserve the prior enum assignment and must not be emitted. Starting an accepted test replaces the prior
retained test result: it sets the target,
zeros progress counters, timings, mismatch count, execution error, and filesystem result, then publishes Queued.
Rejected starts preserve the prior test result and update only `self_test_request_error`. Cancelling a running test
retains its partial counters and timings in the terminal Cancelled state; cancelling when no test is active is a
completed no-op and does not erase the retained result. Terminal results remain available until a new test is
accepted or the device reboots; the report does not automatically return to Idle after a terminal result. After
reboot, Idle has zero target, counters, timings, and execution error. Queued reports zero progress and timing
counters. During execution, counters are cumulative for the current run; after a terminal state they are final. The
filesystem-remains-mounted flag reports actual mount state at the time of the snapshot. Completion follows cleanup
attempts. The test rejects an existing `/PFTEST.BIN`; it does not replace the file.

Each report sequence indicates publication only. Storage and self-test snapshots are sampled independently and are
not an atomic board-wide snapshot. `sd_flags` bit 6 distinguishes a valid SD sample from a temporarily unavailable
one. GET does not advance either Input sequence.

## Report ID 20: Control Command & Result

Feature report `20` is the command surface for LED, reboot, MSC, self-test, and saving persistent settings. SET
submits one typed operation. GET returns the latest operation result and any data produced by that operation.
Read stored settings from Feature report `2`. Both SET and GET payloads are exactly 9 bytes; the operation determines
the meaning of the fields.

### SET Request

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Command layout version, `1`. |
| 1 | 1 | `transaction_id` | Host-chosen nonzero ID; echoed by GET. |
| 2 | 1 | `opcode` | Operation from the table below. |
| 3 | 1 | `flags` | Opcode-specific; all unspecified bits zero. |
| 4 | 5 | `arguments` | Opcode-specific arguments; unused bytes zero. |
| **Total** | **9** | | **10 bytes including report ID.** |

| Opcode | Operation | Flags | Arguments |
| ---: | --- | --- | --- |
| `1` | Set LED | `0` | Byte 0: `0` off or `1` on; bytes 1-4 zero. |
| `2` | Reboot | Bit 0 must confirm; other bits zero. | All zero. |
| `3` | MSC control | Bit 0 confirms force-return data-loss risk only; other bits zero. | Byte 0 action: `0` normal disable, `1` enable, `2` force return; bytes 1-4 zero. |
| `4` | SD self-test | `0` | Byte 0 action: `0` cancel, `1` start; bytes 1-4 little-endian MiB size. |
| `5` | Unassigned | - | Reject as unsupported. |
| `6` | Save persistent settings and reboot | Bit 0 confirms forced-save host-cache data-loss risk; other bits zero. | Bytes 0-1 system-clock MHz; byte 2 SD-card profile (`0` Standard, `1` High-Speed); bytes 3-4 zero. |
| Other | Unassigned | - | Reject as unsupported. |

LED Off/On are idempotent. Reboot requires explicit confirmation. MSC action `0` is the normal disable request;
the host application is responsible for safely ejecting first, and the device checks its SCSI eject indication
(unless media removal is confirmed), removal prevention, and device sync without claiming host-cache durability.
Action `2` is force return without a safe-eject guarantee and requires explicit data-loss confirmation. Self-test
start accepts 1-1024 MiB; cancel requires zero size. System-clock values are `0`, `150`,
`200`, or `250` MHz; SD-card profile values are `0` Standard and `1` High-Speed. A successful settings save schedules
reboot automatically; verify the saved values in the matching Feature `20` result, then refresh Feature `2` after
reboot. Do not submit a second reboot command.

### GET Result

GET returns the latest operation without executing it again or consuming its result.

| Offset | Size | Field | Meaning |
| -----: | ---: | ----- | ------- |
| 0 | 1 | `version` | Command layout version, `1`. |
| 1 | 1 | `transaction_id` | ID associated with the latest accepted result; `0` before any request. |
| 2 | 1 | `opcode` | Operation associated with the result; `0` if none. |
| 3 | 1 | `status` | Result state below. |
| 4 | 1 | `error_reason` | Error enum below; zero when none. |
| 5 | 4 | `result_data` | Verified saved settings for opcode 6; zero for other operations. |
| **Total** | **9** | | **10 bytes including report ID.** |

Status values: `0` Idle, `1` Pending, `2` Completed, `3` Invalid, `4` Unsupported, `5` Busy, `6` Failed.
Error values: `0` none; `1` malformed opcode-specific flags or unused arguments; `2` missing confirmation;
`3` invalid operation argument or setting; `4` unavailable capability; `5` busy; `6` operation failed;
`7` unsupported opcode. Status/error combinations are restricted as follows:

| Status | Allowed error | Meaning |
| --- | --- | --- |
| `0` Idle | `0` | No result: opcode and transaction ID are zero. |
| `1` Pending | `0` | A recognized request is being processed. |
| `2` Completed | `0` | Operation completed, or an asynchronous operation was accepted and queued. |
| `3` Invalid | `1`, `2`, `3` | Request has a recognized opcode and transaction ID but invalid flags, arguments, or missing confirmation. |
| `4` Unsupported | `4`, `7` | Operation is not available under current capabilities, or opcode is unassigned. |
| `5` Busy | `5` | Operation cannot start while a conflicting operation owns required resources. |
| `6` Failed | `6` | Operation processing began but could not complete, including a self-test start preflight failure. |

Opcode-specific results: opcode `1` permits Pending, Completed, Invalid (`1`/`3`), Unsupported (`4`), or Failed;
opcode `2` permits Pending, Completed, Invalid (`1`/`2`), Unsupported (`4`), Busy, or Failed; opcode `3` permits
Pending, Completed, Invalid (`1`/`2`/`3`), Busy, or Failed; opcode `4` permits Pending, Completed, Invalid (`1`/`3`),
Unsupported (`4`), Busy, or Failed; opcode `6` permits Pending, Completed, Invalid (`1`/`3`), Unsupported (`4`),
Busy, or Failed. Opcode `4` returns Unsupported (`4`) when Report `1` bit 5 is clear. Opcode `3` is supported
whenever either selected-transport bit 3 or 4 is set and must not return Unsupported (`4`) on the basis of a
separate MSC capability. Opcode `5` and unknown opcodes return Unsupported (`7`).
For opcode `4`, Invalid/error `3` reports an invalid size and Report `11` records request error `4`; Busy/error `5`
reports request error `1` (conflicting work) or `2` (MSC ownership). Failed/error `6` reports preflight request
errors `5` (card unavailable), `6` (insufficient space), `7` (test file exists), `8` (preflight file-check I/O), or
`11` (filesystem not mounted). These preflight failures occur after command processing begins but before the test is
accepted; they are not queued and do not replace the retained test result. Report `11` updates
`self_test_request_error` with the specific self-test error. Once preflight succeeds and the test is accepted, the
command result is Completed/error `0` (or Pending/error `0` while acceptance is being processed), and Report `11`
publishes Queued and owns subsequent progress and terminal state. Completed for opcode `3` means the MSC transition
was accepted/queued; for opcode `4` it means the self-test was accepted/queued. Their progress and terminal state
are reported by `10` and `11`, respectively.

A SET with an invalid report length, version, or zero transaction ID is rejected without changing the last result;
the host must treat the SET failure as authoritative and must not wait for a matching GET result. Other invalid
requests with a valid envelope update the result slot with the matching transaction ID and Invalid status. Unknown
status or error values and disallowed status/error/opcode combinations are protocol errors.

For opcode 6, `Completed` means the write passed readback verification and `result_data` contains system-clock MHz
(little-endian two bytes) and the SD-card profile in byte 2; byte 3 is zero. Feature report `2` is the authoritative
read path for persisted settings. For opcode 3, completion means the MSC operation was accepted/queued; report `10`
owns its state. For opcode 4, completion means the self-test operation was accepted/queued; report `11` owns its
progress and terminal status. For reboot, completion means reset was scheduled, not that the host observed a
successful reboot.

An accepted opcode `3` transition waits for the matching Feature `20` result GET to finish before disconnecting HID.
If the host does not read the result, the transition proceeds after a bounded 500 ms grace period. This allows the
host to observe acceptance without making ownership depend indefinitely on a GET.

There is one result slot. Only one host should issue writes at a time, and it must serialize requests: do not submit
another command until the prior command's result is terminal and any associated asynchronous lifecycle is understood.
A later accepted operation replaces the prior result. The host should advance transaction IDs through `1`-`255`,
never reuse an ID while an earlier request using it could still be outstanding or confused with the current result,
and match both transaction ID and opcode in GET. IDs do not provide multi-client locking or deduplication. A
timeout/disconnect makes outcome uncertain; do not automatically replay reboot, save, or force-return requests, and
do not send another command while an earlier request's outcome remains uncertain. GET is non-destructive and never
extends or restarts a pending operation.

### Confirmed Save and Reboot While Shared

Opcode `6`, flags `0`, retains the normal firmware-owned storage requirement. Flags `1` is only valid with MSC
enabled and requires capability bit 6 plus an explicit host data-loss confirmation. Both paths reject outstanding
worker work and ownership transitions. Standalone opcode `2` still rejects MSC ownership.

The forced path first returns Pending with zero result data. The USB task waits for the host to fetch Pending
and a 10 ms control-transfer settle interval, or a 500 ms fallback deadline, then rechecks safety gates. It reserves
storage through successful device-side sync and USB detach before flash programming. A failed sync/detach causes
Failed/error `6`, with no flash save or reboot. A flash-save failure restores the prior RAM profiles, reconnects host
access, and reports Failed without reboot; flash may nevertheless have partially changed.

After verified saving, reconnect HID without usable MSC and keep media USB-reserved until reset so neither the host
nor the worker starts new card I/O. Publish Completed with the original transaction and verified settings.
Reboot waits for that result GET, or a five-second fallback deadline, followed by the existing one-second USB result
grace period. Boot then follows the [default-mounted storage lifecycle](../architecture.md#storage-ownership-lifecycle).
Additional control writes while the forced transaction is outstanding are ignored with a UART warning to preserve
its result slot; hosts must serialize writes. GET never replays a save.

Clients reconnect only to the same verified device UID and poll the original opcode/transaction result. Never replay
SET or infer completion from reconnection. Timeout, mismatched identity, or invalid result leaves outcome uncertain
and blocks further writes. The [settings safety policy](../settings.md#forced-save-and-reboot) explains confirmation
and host-cache risks. This workflow has host/model coverage, not physical USB/power-loss qualification.

### Cross-Interface Resource Arbitration

Storage arbitration is device-wide, not per USB interface. One storage transaction or background storage lifecycle
owns the shared SD-card/filesystem resource at a time. This includes CDC filesystem commands (including reads), HID
self-test (opcode `4`), MSC ownership transitions (opcode `3`), CDC fault-recovery requests, and automatic
initialization or mounting. An operation retains ownership through its cleanup and completion/reply handling; an
MSC transition retains it until the transition and any remount attempt settle. A conflicting request from either
interface is rejected as busy and is not queued for later execution.
Report `20` uses Busy/error `5` where the opcode permits it; CDC must return an explicit busy result, not a
success-shaped queued acknowledgement. The host must wait for the active operation to finish and submit a new request
if appropriate.

Read-only diagnostics that do not access card data may coexist with storage work, subject to the interface's normal
response handling. No request may access the SD card through firmware while MSC owns it. This arbiter does not replace
the single HID result-slot rule: hosts still serialize HID writes, and the device also rejects conflicting work
originating from CDC.

### MSC Force-Return Safety

MSC is disabled at boot; firmware automatically mounts configured media according to the
[storage lifecycle](../architecture.md#storage-ownership-lifecycle). Only explicit USB enable transfers ownership;
starting a self-test or opening a dashboard does not. For opcode `3`, action `1` enables MSC only when the SD card is
initialized and no storage command or reply is outstanding. If firmware has mounted the filesystem, the storage worker unmounts it before
ownership transfer. A successful unmount pauses automount before USB ownership moves to the host. An unmount failure
rejects the request, leaves firmware ownership intact, and does not format the card. Both enable and disable requests
are rejected while unrelated storage work or replies are outstanding. Repeating action `1` while MSC is already
enabled completes as a no-op; repeating action `0` while MSC is already disabled completes as a no-op and does not
retry a failed filesystem mount. Action `2` is only valid while MSC is enabled; otherwise reject it as an invalid
operation argument. A successful ownership transition briefly disconnects USB and re-enumerates; HID is unavailable
during that interval.

Action `0` is the normal disable request, intended after the user safely ejects the volume through the host operating
system. Firmware requires its SCSI eject indication (unless removal is confirmed) and rejects host removal prevention
or failed device-side sync. These checks cannot establish host-cache durability or flush host-cached writes.
The HID client only delivers the request and must not invoke OS eject/unmount commands.
Action `2` is force return and requires both the report's confirmation flag and the dashboard's explicit data-loss
warning/confirmation. It does not synthesize a successful SCSI eject. For either disable action, firmware rejects a
busy transition or failed device-side sync before detach; in that case MSC remains enabled and host-owned.

After action `0` passes eject/sync checks, ownership returns to firmware and USB re-enumerates without MSC.
The storage worker automatically reinitializes and remounts under the normal lifecycle policy, including paced
retries on mount failure unless an earlier recovery block remains active. The transition flag can clear before this
background mount completes; hosts must verify fresh mounted storage status before allowing file operations or Start.

Action `2` disconnects USB before releasing SD ownership. The storage worker makes one initialization and mount
attempt, and `msc_transition_pending` remains set until that attempt finishes; pending is clear in either terminal
outcome. On success, MSC is disabled and firmware owns the mounted storage. On initialization or mount failure,
MSC remains disabled, `msc_request_rejected` is set, and storage remains unmounted with automatic retries blocked
until an explicit successful `fs mount`. No automatic
formatting, filesystem repair, or failed-write replay is performed. A connected HID device after re-enumeration does
not prove that the filesystem remounted; hosts wait for pending to clear, inspect rejection, and verify storage
state. Faulted-media recovery is a separate explicit workflow and is not a clean eject.

Device-side sync cannot flush host-cached writes. Force return may lose data or corrupt the filesystem even if
PicoForge later mounts successfully. Stop host file activity and manually safely eject whenever possible before
requesting either disable action. If a pre-detach operation fails, the host still owns the card. If the post-detach
mount fails, firmware owns the card but leaves it unmounted; the host must not infer recovery from USB reconnection.
The card is never automatically formatted or repaired.

## Host Polling and Validation

1. Open HID and read Feature `1`. Validate the three-byte product prefix, product/layout, exact payload length,
   reserved fields, enums, capabilities, and SRAM bounds before enabling PicoForge-specific behavior. Read Feature
   `2` for the persisted next-boot clock and SD-card profiles, reset cause, and GPIO configuration; use Feature
   `1`'s transport capability to interpret the storage GPIOs.
2. Start Input monitoring for reports `3` and `10`; also monitor report `11` only if Report `1` capability bit 5 is
   set. If bit 5 is clear, do not subscribe to Report `11`, expect it to be published, or enable opcode `4`. Track
   freshness independently. A valid storage report can have an unavailable SD sample; honor `sd_flags` bit 6 and do
   not infer that zero card values mean an empty card.
3. Submit all writable operations through report `20` with a fresh nonzero transaction ID. Read GET until the matching
   result is terminal. For opcode `3`, `Completed` means the transition was accepted/queued, not that ownership
   changed: after any USB re-enumeration, reopen the same authorized HID device and monitor report `10` until
   `msc_transition_pending` clears, then inspect `msc_enabled` and `msc_request_rejected`. After return, also verify
   fresh mounted filesystem status; normal-disable completion can precede background remount. Do not retry an uncertain
   non-idempotent operation.
4. Use report `10` for storage/MSC state and, when capability bit 5 is set, report `11` for self-test progress and
   terminal status; SET acknowledgement is not operation completion. For MSC re-enumeration, reopen the same
   authorized device and reread reports `1` and `2`.
5. Read persisted settings from Feature `2`. To save and apply them, confirm with the user, issue opcode `6`, and
   verify the saved values in its completion result. Firmware reboots automatically only after a successful save.
   Reconnect and reread Feature `2`; opcode `2` remains available for a standalone reboot.

Validate report type, ID, exact length, version, reserved fields, enums, and cross-field constraints. Check that
Report `1` selects exactly one storage transport and that its assigned board model agrees with the MCU; Report `2`'s
stored profiles are valid; and Report `10`'s transport, active speed mode, SD validity bits, zeroed invalid fields,
and MSC booleans are consistent. Unsupported layouts and failed mandatory reads are explicit errors, not successful
zero-filled fallbacks. Report `10` includes the full card CID, including its serial number; hosts should only display
or log that identifier when needed and must not transmit it to unrelated services.

### Input Freshness

Hosts timestamp each valid Input report on receipt using a monotonic clock and track freshness independently by
report ID. A report remains fresh for three times its nominal publication interval: 750 ms for report `3`, and
1500 ms for reports `10` and `11`. At or beyond that age, show the report as stale and do not present its values as
current. Before the first valid report, its state is unavailable. A valid current-snapshot GET may refresh that
report's host receipt time, but does not advance its Input sequence.

On HID suspend, mark Input data stale until fresh reports or valid snapshot reads resume. On disconnect, clear the
connection's cached report state; USB re-enumeration does not make old values fresh. After reconnect, require new
valid report data (and repeat mandatory Feature reads) before displaying current state or enabling commands. Hosts
may show prior values as explicitly historical, but must label them stale.

## Design Validation

Each report layout starts at version `1`. Increment the affected report's version when an incompatible field, length,
unit, validation rule, or lifecycle changes. Layout versions are independent of firmware semantic version.

Conformance tests cover report types and lengths, API report-ID framing, field validation, stale telemetry,
configuration cross-field rules, storage-state validity and ownership transitions, profile migration for the
build-selected transport, command validation and results, cross-interface resource arbitration, self-test result
retention, CID decoding, and PicoUART prefix compatibility. Host tests and firmware cross-builds do not verify
physical USB behavior. Qualify CDC, HID, MSC transitions, and re-enumeration on RP2040 and RP2350 hardware before
product use.

## Implementation and Integration

The descriptor and firmware implementation are in `firmware/src/app/usb`; storage self-test admission and execution
are in `firmware/src/app/storage`. The Python, .NET, shared dashboard, and direct WebHID clients implement the same
report contract. Settings saves preserve the existing persistent-record format and update only the selected
transport's legacy speed field; Report `2` exposes the transport-neutral profile. An accepted self-test is
preflighted by the sole storage worker before its prior result is replaced. The reserved `/PFTEST.BIN` path is never
overwritten. MSC transitions allow a bounded interval for the matching Feature `20` result GET before a disconnect.

This describes software implementation, not hardware qualification. HID report delivery, MSC ownership changes,
host safe-eject behavior, and reconnect handling still require physical validation on the supported boards.
