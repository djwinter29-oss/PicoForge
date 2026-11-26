# User Guides

[Documentation](../README.md)

Already running firmware? Pick the operation below. For your first board, start with
[Getting Started](../getting-started.md).

## Dashboards

| Dashboard | Choose it when... | Install and connect |
| --- | --- | --- |
| Direct | You use desktop Chrome/Edge and want USB or provisioned BLE access without a host app | [Direct guide](../../host/direct/README.md) |
| Python | You want a local USB HID dashboard or command-line diagnostics | [Python guide](../../host/python/README.md) |
| .NET | You want a self-contained native USB HID host | [.NET guide](../../host/dotnet/PicoForge/README.md) |

Run only one dashboard/HID monitor per board. The component guides own their prerequisites and launch commands.

## Board Operations

| Task | Guide |
| --- | --- |
| Wire an SD breakout, monitor UART, or debug probe | [Hardware setup](hardware.md) |
| Read files, share the card over USB, or safely remove it | [SD-card storage](storage.md) |
| Use CDC/TCP commands, format media, or recover faulted MSC | [Console](console.md) |
| Provision Wi-Fi and a Control key, then connect over BLE | [Wireless](wireless.md) |
| Run a storage benchmark and interpret its results | [SD-card benchmark](sdcard-benchmark.md) |
| Understand next-boot clock, storage-speed, and radio settings | [Boot settings](../reference/settings.md#host-dashboard-settings) |

For code changes or test commands, use [Development](../development/README.md). For byte layouts and internal
interfaces, use [Technical Reference](../reference/README.md).
