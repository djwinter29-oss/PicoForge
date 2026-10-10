# PicoForge USB Status Protocol

## Device Interfaces

The development composite device uses VID/PID `0xCAFE:0x4011`. Replace this unallocated development identity before product release.

- CDC ACM is the serial console; firmware sends `PicoForge CDC ready` after the host asserts DTR.
- HID uses vendor usage page `0xFF00`, usage `0x01`.
- USB product string: `PicoForge CDC + HID`.
- USB serial string: the Pico SDK unique board ID.

The HID report ID is the first byte of each report delivered by host APIs. Multi-byte integer fields are unsigned little-endian. Feature-report APIs may prepend an additional zero report-number byte on some Linux transports; host decoders accept that transport prefix and zero-filled report padding.

## Feature Report 1

Feature report 1 has a 26-byte payload, excluding the report ID.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `1` |
| 1 | 1 | MCU ID | `1` = RP2040, `2` = RP2350 |
| 2 | 4 | `clk_sys` | Unsigned Hz, little-endian |
| 6 | 20 | Product name | NUL-padded ASCII, currently `PicoForge` |

Hosts read this feature report on connect and refresh.

## Input Report 2

Input report 2 has a 7-byte payload and is published every 250 ms.

| Payload offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | Layout version | `1` |
| 1 | 1 | Sequence | Increments for each accepted periodic report |
| 2 | 4 | Uptime | Unsigned milliseconds, little-endian; wraps after `2^32` ms |
| 6 | 1 | Flags | Bit 0: USB mounted; bit 1: CDC DTR/open; other bits reserved |

The sequence byte wraps modulo 256. The feature report also supports `GET_REPORT`, returning the current hardware information; the input report supports `GET_REPORT` with the current runtime values.

## Host Implementations

- Python HIDAPI client and loopback Flask dashboard: `host/python/`.
- .NET 10 HidSharp monitor and loopback dashboard/API: `host/dotnet/`.
- Direct browser WebHID dashboard: `host/webhid/`, sharing the display code from `host/web/`.

Firmware cross-builds and hardware-free host tests do not verify physical host enumeration. Test the composite interface and CDC/HID behavior on RP2040 and RP2350 boards before relying on it in a product.