# PicoForge .NET Host

Monitor PicoForge boards over USB HID and use the local dashboard at <http://127.0.0.1:5000/>.

## Run a Released Package

1. Download the matching self-contained host archive from the
   [latest release](https://github.com/djwinter29-oss/PicoForge/releases/latest):
   Linux x64, Windows x64 or macOS ARM64. Verify its checksum and extract the entire archive; see
   [download verification](../../../docs/deployment/release.md#using-downloads).
2. Install native HID dependencies if needed:
   - Debian/Ubuntu: `sudo apt install libhidapi-hidraw0`; configure
     [HID permissions](../../../tools/host/hid-permissions-setup.sh) and reconnect the board.
   - macOS: `brew install hidapi`.
3. Run `PicoForge.exe` on Windows or `./PicoForge` on Linux/macOS, then open <http://127.0.0.1:5000/>.
   Pass `--port 5001` if another application uses port 5000.

Packages include the .NET runtime and dashboard assets; no .NET SDK/runtime installation is required.
Binaries are unsigned; follow local security policy and run
only trusted, verified downloads if SmartScreen or Gatekeeper warns.

## Run from Source

Install the .NET 10 SDK, then run from the repository root:

```sh
tools/host/run-dotnet.sh
# Or:
dotnet run --project host/dotnet/PicoForge/PicoForge.csproj
```

The launcher checks Linux HID permissions and forwards application options:

```sh
tools/host/run-dotnet.sh --port 5001
tools/host/run-dotnet.sh --serial SERIAL
tools/host/run-dotnet.sh --device-path HID_PATH
```

Serial and device-path selection are mutually exclusive. Without either, the first discovered board is selected.

## Connect and Check

Connect a board running PicoForge firmware with a data-capable USB cable. Open the dashboard, use **Rescan**, and select
the intended board. Identity/configuration may briefly show as waiting while monitoring starts; Live diagnostics should
then show current board identity and available telemetry. Storage and radio details depend on board capabilities.

Use only one dashboard/HID monitor per board; close competing Python, .NET or Direct connections.
Python and .NET share the frontend in `host/web/src`; Direct is an independent implementation under the
[Direct independence policy](../../../docs/development/host-tools.md#direct-dashboard-independence).

## Troubleshooting and Safety

| Symptom | Action |
|---|---|
| Server cannot start on port 5000 | Stop the other server or pass `--port 5001`. |
| Board is missing or HID access fails | Check the data cable, firmware, native HID dependency and Linux permissions; reconnect and Rescan. |
| Board is waiting, stale or disconnected | Allow initial discovery, check USB access and close competing monitors. Do not treat stale telemetry as current. |
| Duplicate identity warning | Leave only the intended board attached and Rescan; absence alone does not clear quarantine. |
| Command outcome is uncertain | Use **Re-sync** and follow recovery instructions. Do not repeat the write to guess whether it succeeded. |
| Wi-Fi cleanup is required | Follow **Clear Wi-Fi test** recovery; unrelated writes remain blocked until cleanup succeeds. |
| Native HID cleanup fails | Restart the host; it deliberately does not open a replacement handle after failed cleanup. |

Before changing storage or persistent settings, read the relevant safety procedure:

- [USB storage ownership and safe eject](../../../docs/guides/getting-started.md#5-try-sd-card-storage)
- [Force-return risks](../../../docs/reference/usb-hid.md#msc-force-return-safety)
- [Forced Save&Reboot and data-loss confirmation](../../../docs/reference/settings.md#forced-save-and-reboot)
- [SD-card benchmark safety](../../../docs/guides/sdcard-benchmark.md)
- [Wi-Fi TCP console setup and security](../../../docs/guides/console.md#wi-fi-tcp-terminal)

## Develop and Test

| Folder | Responsibility |
|---|---|
| `Hosting/` | Startup and dependency registration |
| `Protocol/` | HID models, validation and codecs |
| `Hid/` | HID transport and serialized monitoring |
| `Dashboard/` | Board selection, HTTP endpoints and TCP console bridge |

Build/publish copies current `host/web/src` assets into `wwwroot`; edit shared source, not generated copies.
Run hardware-free tests from the repository root:

```sh
dotnet test host/dotnet/PicoForge.sln --configuration Release --filter "Category!=Performance"
```

For API exploration, set `ASPNETCORE_ENVIRONMENT=Development` to expose `/openapi/v1.json`.
Requests select a board with `?board=<id>` from `GET /api/boards`; `GET /api/status` returns its current snapshot.
The [host development guide](../../../docs/development/host-tools.md) owns shared frontend checks,
[API contracts](../../../docs/development/host-tools.md#shared-api-contract),
[command ownership](../../../docs/development/host-tools.md#command-ownership-and-cancellation) and
[.NET lifecycle design](../../../docs/development/host-tools.md#net-host-lifecycle).
The [HID reference](../../../docs/reference/usb-hid.md) owns the wire protocol.
Passing host tests does not establish physical USB/storage reliability; use the
[.NET hardware procedure](../../../docs/development/host-tools.md#qualify-the-net-host-with-hardware).
