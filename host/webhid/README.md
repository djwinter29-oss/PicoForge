# PicoForge WebHID

Serve the WebHID directory directly so its `index.html` is the site root and the browser gets a secure localhost origin:

```sh
python3 -m http.server 8000 --directory host/webhid
```

Open `http://localhost:8000/` in a WebHID-capable browser, connect the board, and grant access to its vendor HID interface. When MSC is enabled or disabled, USB briefly re-enumerates; the dashboard automatically reopens the same authorized HID device by board serial. **Disable USB drive** requires data-loss confirmation, then asks Pico to detach and attempt a remount without OS eject commands. See the [return contract and risks](../../docs/usb-protocol.md#confirmed-force-return). The page owns its HTML, CSS, view logic, and favicon. It shares only the HID wire protocol with the Python and .NET hosts. USB serial support in a browser is not used; CDC remains available to terminal applications.

The [SD self-test](../../docs/sd-self-test.md) uses HID Report 6 to start/cancel
a verified temporary-file benchmark and receive progress/timing metrics. Default
size is 32 MiB; selecting Start authorizes temporary-file writes and replacement.
The first summary row includes the firmware release version (`Unknown` for legacy firmware).
Runtime status shows Core 0 and Core 1 CPU usage separately, plus distinct reserved and total SRAM values; system
clock appears only in the summary row. See [runtime telemetry definitions and compatibility](../../docs/usb-protocol.md#input-report-2).
LED control is in Live Report; USB-drive control is beside the self-test. Do not
run a competing Python/.NET HID monitor for the same board at the same time.

Windows may deliver Input reports in buffers padded to the collection's maximum Input payload (32 bytes).
The WebHID event handler decodes only each report's defined payload; transport-tail bytes are ignored.
Payload fields, including reserved bytes, remain validated. Feature-report framing is handled separately.