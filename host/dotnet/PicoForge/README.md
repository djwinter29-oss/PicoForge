# PicoForge .NET Host

The .NET 10 host monitors the vendor HID interface and serves the shared local dashboard at `http://127.0.0.1:8766/`.

```sh
dotnet run --project host/dotnet/PicoForge/PicoForge.csproj
```

The loopback-only `GET /api/status` endpoint returns hardware identity, runtime telemetry, board temperature and LED state, plus SD-card presence, transport, effective bus speed, capacity, CID identity, and MSC state. `POST /api/msc` accepts `{"enabled":true}` or `{"enabled":false}`. Enabling briefly re-enumerates USB with a host-visible drive while keeping HID identity stable; safely eject the drive before disabling to return the card to PicoForge. Firmware storage must be unmounted before handoff. `POST /api/led` accepts `{"on":true}` or `{"on":false}` to set the board LED when one is configured. `POST /api/self-test` accepts `{"action":"start","size_mib":32}` or `{"action":"cancel"}` and acknowledges HID delivery with HTTP 202; `self_test` in the status snapshot carries results. See [the self-test guide](../../../docs/sd-self-test.md) for safety and metric definitions. Run with `ASPNETCORE_ENVIRONMENT=Development` to expose the OpenAPI document at `/openapi/v1.json`.