# PicoForge USB Status Protocol

## Device Interfaces

The development composite device uses VID/PID `0xCAFE:0x4011`. Replace this unallocated development identity before product release.

- CDC ACM is the serial console; firmware sends `PicoForge CDC ready` after the host asserts DTR.
- HID uses vendor usage page `0xFF00`, usage `0x01`.
- USB product string: `PicoForge CDC + HID`.
- USB serial string: the Pico SDK unique board ID.

The HID report ID is the first byte of each report delivered by host APIs. Multi-byte integer fields are little-endian. Feature-report APIs may prepend an additional zero report-number byte on some Linux transports; host decoders accept that transport prefix and zero-filled report padding. Hosts read Feature Report 1 once when connecting for stable identity, then use periodic Input Report 2 for changing values. Report ID 3 is reserved and not currently described or sent.

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

## Feature Report 4

Feature report 4 accepts a 2-byte command payload: version `2`, then desired LED state (`0` off or `1` on). Setting a state is idempotent; the command is ignored if malformed or if the selected board has no controllable LED. The resulting state is reported in subsequent Input Report 2 telemetry.

## Host Implementations

- Python HIDAPI client and loopback Flask dashboard: `host/python/`.
- .NET 10 HidSharp monitor and loopback dashboard/API: `host/dotnet/`.
- Direct browser WebHID dashboard: `host/webhid/`, sharing the display code from `host/web/`.

Firmware cross-builds and hardware-free host tests do not verify physical host enumeration. Test the composite interface and CDC/HID behavior on RP2040 and RP2350 boards before relying on it in a product.