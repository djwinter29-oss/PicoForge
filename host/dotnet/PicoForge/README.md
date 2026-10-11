# PicoForge .NET Host

The .NET 10 host monitors the vendor HID interface and serves the shared local dashboard at `http://127.0.0.1:8766/`.

```sh
dotnet run --project host/dotnet/PicoForge/PicoForge.csproj
```

The loopback-only `GET /api/status` endpoint returns hardware identity, runtime telemetry, board temperature and LED state, plus SD-card presence, transport, effective bus speed, capacity, CID identity, and MSC state. `POST /api/msc` accepts `{"enabled":true}` or `{"enabled":false}`. Enabling disconnects/re-enumerates with a host-visible drive; safely eject the drive before disabling. Firmware storage must be unmounted before handoff. `POST /api/led` accepts `{"on":true}` or `{"on":false}` to set the board LED when one is configured. Run with `ASPNETCORE_ENVIRONMENT=Development` to expose the OpenAPI document at `/openapi/v1.json`.