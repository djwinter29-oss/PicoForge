# PicoForge .NET Host

The .NET 10 host monitors the vendor HID interface and serves the shared local dashboard at `http://127.0.0.1:5000/` by default. Override the port with `tools/host/run-dotnet.sh --port PORT`; only one dashboard server can bind to port 5000 at a time.

```sh
dotnet run --project host/dotnet/PicoForge/PicoForge.csproj
```

`GET /api/boards` discovers attached boards. The dashboard's **Rescan** and board selector choose one, and status/command requests include its ID as `?board=<id>`. `GET /api/status` returns the selected board's hardware identity, runtime telemetry, LED state, SD-card information, and MSC state. `POST /api/led` sets the selected board's LED; `POST /api/settings` saves supported boot settings and verifies HID readback. `POST /api/msc` accepts `{"enabled":true}` or `{"enabled":false}`. Enabling briefly re-enumerates USB while keeping HID identity stable; safely eject before disabling to return the card to PicoForge. `POST /api/self-test` starts or cancels the SD-card test and acknowledges HID delivery with HTTP 202. See [the performance-test guide](../../../docs/design/sdcard/sdcard-performance-test.md) for safety and metric definitions. Run with `ASPNETCORE_ENVIRONMENT=Development` to expose `/openapi/v1.json`.
The dashboard's **Return to Pico** action requires explicit data-loss confirmation and requests Pico-side
detach/remount without any host OS eject command. See the
[force-return design](../../../docs/design/usb/usb-hid-report-reference.md#msc-force-return-safety) for its
one-attempt remount policy and host-cache risks. The legacy safe-disable API remains available for manual-eject clients.
If a HID write times out or its result cannot be matched, the dashboard marks the command outcome uncertain and blocks
further writes rather than replaying or overwriting the result.