# Technical Reference

[Documentation](../README.md)

Use these pages for exact behavior and interfaces. For operating steps, use the [user guides](../guides/README.md);
for build and test workflows, use [Development](../development/README.md).

## Firmware

| Look up... | Authoritative reference |
| --- | --- |
| Layers, runtime tasks, storage ownership, automount, and worker admission | [Architecture](architecture.md) |
| Compile-time options, optional modules, and valid combinations | [Build configuration](build-configuration.md) |
| Next-boot settings, flash record, load/stage/save, and forced-save safety | [Boot settings](settings.md) |
| SD protocol, block interfaces, ports, resource use, and recovery | [SD-card driver](sdcard/driver.md) |
| PIO/SPI engine lifecycle, pin ownership, clocks, and wire timing | [SD-card physical links](sdcard/physical-links.md) |

## Host and Wire Contracts

| Look up... | Authoritative reference |
| --- | --- |
| USB report layouts, capabilities, command results, and MSC safeguards | [USB HID](usb-hid.md) |
| BLE advertising, GATT, authorization, command framing, and errors | [Bluetooth LE](bluetooth.md) |
| Radio execution, credential staging, and TCP proof generation | [Wireless integration](../development/wireless-integration.md) |
| Native-host HTTP routes and asynchronous command ownership | [Host API contracts](../development/host-tools.md#shared-api-contract) |
| Direct session design, polling, and freshness | [Direct development](../development/direct-dashboard.md) |

Implementation contracts describe current behavior, not hardware qualification. Consult
[known limits and evidence](../evidence/README.md) before relying on a particular board/card/workload combination.
