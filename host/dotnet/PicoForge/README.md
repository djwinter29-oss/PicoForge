# PicoForge .NET Host

The .NET 10 host monitors the vendor HID interface and serves the shared local dashboard at `http://127.0.0.1:8766/`.

```sh
dotnet run --project host/dotnet/PicoForge/PicoForge.csproj
```

The loopback-only `GET /api/status` endpoint returns hardware identity, system clock, report sequence, uptime, USB/CDC state, board temperature, and LED availability/state. `POST /api/led` accepts `{"on":true}` or `{"on":false}` to set the board LED when one is configured. Run with `ASPNETCORE_ENVIRONMENT=Development` to expose the OpenAPI document at `/openapi/v1.json`.