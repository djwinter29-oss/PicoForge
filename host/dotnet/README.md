# PicoForge .NET Host

The .NET 10 host monitors the vendor HID interface and serves the shared local dashboard at `http://127.0.0.1:8766/`.

```sh
dotnet run --project host/dotnet/PicoForge.Host.csproj
```

The read-only `GET /api/status` endpoint returns hardware identity, system clock, report sequence, uptime, and CDC state. The server binds to loopback only. Run with `ASPNETCORE_ENVIRONMENT=Development` to expose the OpenAPI document at `/openapi/v1.json`.