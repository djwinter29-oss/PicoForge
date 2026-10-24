# USB CDC/HID Overview

PicoForge exposes a composite USB device with a CDC ACM interface and a vendor-defined HID interface. USB Mass
Storage Class (MSC) is added when the SD card is shared with the host. The interfaces have separate responsibilities:

| Interface | Purpose | Host use |
| --- | --- | --- |
| CDC ACM | Interactive PicoForge command-line interface | Serial terminals and host serial APIs |
| Vendor HID | Board identity, telemetry, SD status, and supported controls | Python, .NET, and WebHID dashboards |
| MSC (optional) | Host access to the attached SD card | Operating-system mass-storage support |

UART debug logging uses the board's monitor UART and is separate from USB CDC. CDC line coding does not configure a
physical UART. HID carries no console text or file data.

## USB Configuration

The development VID/PID is `0xCAFE:0x4011`; replace it before product release. CDC is interface 0 and HID is interface
2 in both configurations. When enabled, MSC is appended as interface 3, so toggling MSC does not change the HID
interface number. USB strings identify the product as `PicoForge CDC + HID` and use the Pico SDK unique board ID as
the serial string.

The HID interface uses vendor usage page `0xFF00`, usage `0x01`, with no boot protocol. Its payload definitions,
report lengths, versions, and command semantics are authoritative in the
[HID Report Reference](hid-report-reference.md). Select that project-specific contract before decoding reports;
matching report IDs or layout versions from another device do not imply compatible payloads.

## HID Report Organization

The implemented HID contract in the [HID Report Reference](hid-report-reference.md) defines six reports:

| Report ID | Role | Contents |
| ---: | --- | --- |
| `1` | Device identity and capabilities | Product identity, firmware, MCU, board model, capabilities, SRAM accounting |
| `2` | Board configuration | Stored clock and SD-card profiles, reset cause, and configured GPIOs |
| `3` | Board telemetry | PicoUART-compatible uptime, clock, LED, temperature, and per-core load; PicoForge USB/CDC flags |
| `10` | Storage status | SD card, filesystem, MSC ownership, active timing mode, and CID |
| `11` | Self-test status | Self-test state and progress |
| `20` | Control command & result | LED, reboot, MSC, self-test, and persistent-settings operations |

## MSC Ownership Changes

MSC is disabled at boot. Enabling it transfers SD-card ownership from firmware to the host; firmware first requires
the card to be initialized and storage work to be idle. When firmware has mounted the filesystem, the storage worker
unmounts it before USB ownership changes. The device briefly disconnects and re-enumerates with MSC enabled.

Disabling MSC normally requires the host to eject the volume first. The dashboard's confirmed **Disable USB drive**
action instead asks PicoForge to detach MSC and attempt to remount the card. It warns about possible data loss because
device-side sync cannot flush writes cached by the host. Stop host file activity and use the host's safe-eject action
whenever possible. Wait for the pending state to clear and inspect the result; USB reconnection alone does not prove
that remount succeeded. These safety principles and the matching result-read grace period are described in the
[MSC command design](hid-report-reference.md#msc-force-return-safety).

## Host Implementations and Validation

- [Python dashboard](../../../host/python/README.md)
- [.NET dashboard](../../../host/dotnet/PicoForge/README.md)
- [Direct WebHID dashboard](../../../host/webhid/README.md)
- [CDC CLI guide](cdc-cli.md)

Firmware cross-builds and host-side tests do not verify physical USB enumeration. The software implementation is in
place, but qualify CDC, HID, and MSC behavior on RP2040 and RP2350 hardware before relying on a product build.
