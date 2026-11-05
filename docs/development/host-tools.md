# Host Tools Development

PicoForge has Python and .NET dashboards that share the frontend in `host/web`, plus the independent Direct dashboard
in `host/direct`, which connects over USB (WebHID) or Bluetooth LE (Web Bluetooth). For installation and everyday use,
see the [Python host guide](../../host/python/README.md), [.NET host guide](../../host/dotnet/PicoForge/README.md),
and [Direct dashboard guide](../../host/direct/README.md).

Run commands from the repository root. Host unit tests do not require a board; physical device checks require an
identified board and appropriate USB permissions.

## Prerequisites

- Python 3.10+ for the Python host and repository contracts
- .NET 10 SDK for the .NET host and tests
- Node.js with the built-in test runner for Direct dashboard tests
- A WebHID-capable desktop browser for physical browser use

Install only the toolchain needed for the host or checks you are changing.

## Run a Host Application

The launchers start the respective dashboard:

```sh
tools/host/run-python.sh
tools/host/run-dotnet.sh
tools/host/run-direct.sh
```

Python and .NET serve the shared dashboard at `http://127.0.0.1:5000/` by default; run only one server on that port at
a time. The Direct dashboard is served at `http://localhost:8000/`. USB connections require WebHID support and local
USB permission. The CDC console is documented in the [CDC CLI guide](../guides/console.md).
The Python/.NET Live diagnostics Wi-Fi link opens a separate web console through the local raw TCP bridge;
setup, session limits and security notes are authoritative in the [CLI guide](../guides/console.md#wi-fi-tcp-terminal).

The Python launcher provisions its environment on first use. To install the package explicitly for development and
tests:

```sh
python3 -m pip install --editable host/python
```

On Linux, HID access can require the repository's
[permission setup script](../../tools/host/hid-permissions-setup.sh), followed by reconnecting the device.

The Python `picoforge status` JSON retains the top-level `msc` field for compatibility; it is deprecated in favor of
`memory.msc`.

## Qualify the Python Host with Hardware

The Python monitor polls HID input reports every 50 ms while a browser polls status, then slows to 500 ms after five
seconds without a status request. Wireless status polling similarly slows to five seconds while idle and resumes its
normal cadence when a browser returns. The monitor releases the device lock between report batches. Host tests verify
these schedules and that commands can acquire the lock between batches, but cannot establish real USB report latency,
CPU use, or device handoff behavior. Before changing these intervals or claiming physical qualification, test the
packaged or source-installed Python host with representative RP2040 and RP2350 boards:

1. Record the OS, Python version, firmware revision, board model, and storage/radio configuration. Start the dashboard
   with `tools/host/run-python.sh` and leave Live diagnostics visible for at least five minutes. Confirm runtime,
   storage, and (where supported) self-test reports stay available and do not become stale during idle operation.
2. Repeat at least ten USB detach/reconnect cycles. Confirm the dashboard shows disconnect/reconnect, recovers to
   current telemetry, and does not leave a stale or duplicate monitor. Repeat while an unrelated board remains
   connected if multi-board operation is in scope.
3. On a board with SD storage, use a backed-up or disposable card. Enable USB storage, verify firmware-owned storage
   controls are blocked during handoff, safely eject from the computer, and return ownership to the board. Exercise
   force-return only when the data-loss warning is understood and the card contains no valuable data.
4. On a W board with a test access point, scan, test a temporary connection, cancel/clear the test credentials, and
   verify the UI and telemetry recover after completion or timeout. Do not use production credentials.
5. Shut down the host while idle and during a wireless test. Confirm shutdown completes after any in-flight HID
   operation and the next launch can reconnect normally.
6. Record worst observed UI/report update delay, monitor process CPU, reconnect result, and any errors. Treat a failed
   step as a qualification gap; do not infer reliability from host unit tests.

Keep dated, configuration-specific observations in the [validation records](../validation/records/README.md), and
summarize outstanding coverage in [qualification gaps](../validation/qualification-gaps.md).

## Qualify the .NET Host with Hardware

The .NET monitor samples HID input every 50 ms; firmware telemetry, storage and self-test reports are emitted every
250 ms, 500 ms and 500 ms respectively. Host tests verify protocol and lifecycle logic, not real report freshness,
multi-board CPU use, or USB/storage durability. Before claiming physical qualification, test the packaged or
source-built host with representative RP2040 and RP2350 boards:

1. Record OS, .NET SDK/runtime, firmware revision, board model, and storage/radio configuration. Run
   `tools/host/run-dotnet.sh` and leave Live diagnostics visible for at least five minutes. Confirm reports remain
   current and do not become stale during idle operation.
2. Repeat at least ten USB detach/reconnect cycles. Confirm the selected board recovers without stale or duplicate
   monitors. If multi-board use is in scope, keep another board connected and verify it remains responsive during
   scans and first-time monitor startup.
3. On a board with SD storage, use a backed-up or disposable card. Enable USB storage, safely eject, return ownership,
   and confirm fresh mounted status. Exercise forced-save recovery only with no valuable data and after reviewing the
   [forced-save policy](../reference/settings.md#forced-save-and-reboot).
4. On a W board with a test access point, scan, test a temporary connection, cancel it, and verify credentials clear
   and the UI recovers after completion or timeout. Do not use production credentials.
5. Measure first `/api/status` response latency and time from the first request to connected status with current
   telemetry. Record worst observed report/UI update delay, host CPU during idle and multi-board operation, command
   response latency, detach/reconnect results, and errors. To compare polling intervals, use otherwise equivalent host
   builds with 10 ms, 50 ms, and (if report freshness permits) 100 ms intervals; keep firmware, board, workload,
   observation duration, and host conditions fixed. Do not infer CPU reduction from wakeup counts alone. Treat failed
   steps as qualification gaps; host tests alone do not establish physical behavior.

Keep dated, configuration-specific observations in the [validation records](../validation/records/README.md), and
summarize outstanding coverage in [qualification gaps](../validation/qualification-gaps.md).

## Run Host Tests

Install the Python package first, then run the three hardware-free host suites together:

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```

The wrapper runs Python `unittest`, separate Direct and Python/.NET frontend Node tests, and .NET tests.
They can also be run individually:

```sh
python3 -m unittest discover -s host/python/tests -v
node --test host/direct/tests/*.test.mjs
node --test host/web/tests/*.test.mjs
dotnet test host/dotnet/PicoForge.sln --configuration Release --nologo --filter "Category!=Performance"
```

Python tests cover the package and local web API; .NET tests cover the protocol and host behavior; Direct tests cover
browser-side decoding and presentation. None opens a physical USB device. Repository-wide Python contracts are separate:

The shared dashboard also has browser-level checks for modal focus and Escape handling, narrow layouts, and Wi-Fi
temporary-test cancellation failure/retry. Serve `host/web` with a static HTTP server and open the check page in a
desktop browser:

```sh
python3 -m http.server 8000 --directory host/web
# Open http://127.0.0.1:8000/tests/browser.html
```

These checks use a mocked board API and do not qualify physical USB, firmware, or radio behavior. Keep device
qualification separate.

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
```

See the [test ownership index](../../tests/README.md) for cross-project checks and their prerequisites.

### Python dashboard performance baseline

Measure the dashboard hot paths before changing polling or snapshot copying. `--board` opens an attached board
read-only; stop any running dashboard first because it holds the HID interface:

```sh
host/python/.venv/bin/python tools/validation/bench-python-dashboard.py [--board [--serial SERIAL]]
```

| Path | Rate | Baseline (2026-11-05, x86-64 Linux) | Revisit when |
|---|---|---|---|
| `snapshot()` deep copy | Each `GET /api/status` | 50-95 us | One status request exceeds 1 ms |
| `GET /api/status` through Flask | 1/s per visible tab | ~0.6 ms | Many tabs or noticeable status latency |
| 50 ms input poll loop | 20 wakes/s per board | 2.4-5.5 ms CPU/s with mocked HID; ~1 ms CPU/s for a running dashboard with a real board | Idle CPU exceeds ~1% per board |

Save, scan and connection-test commands hold the device lock across several USB round trips (see
[command ownership](#command-ownership-and-cancellation)); time them by hand on hardware before splitting that lock, and
split it only where the write-safety rules still hold.

### .NET dashboard performance baseline

`PerformanceTests` times the status hot paths with a mocked board and fails only past ceilings 10-50x the baseline.
It is in the `Performance` category, which the default host suite skips; run it on its own:

```sh
dotnet test host/dotnet/PicoForge.sln -c Release --filter Category=Performance --logger "console;verbosity=detailed"
```

| Path | Rate | Baseline (2026-11-05, x86-64 Linux, Release) | Revisit when |
|---|---|---|---|
| `DeviceMonitor.Snapshot` (freshness applied on read) | Each `GET /api/status` | 0.5-0.9 us | One read exceeds 50 us |
| Status JSON serialization | Each `GET /api/status` | 17-33 us | One status exceeds 0.5 ms |
| `GET /api/status` through Kestrel | 1/s per visible tab | 0.25-0.32 ms | Many tabs or noticeable status latency |

HID commands block one thread-pool thread under the device lock, up to 35 s for a connection test. That is acceptable
for a localhost dashboard; move HID I/O to a dedicated thread only if concurrent requests start queueing.

## Shared Frontend and Protocol Boundaries

Python and .NET serve the same assets from `host/web`; keep shared UI behavior backend-neutral and preserve the common
API response contract. Direct is independent and owns its renderer and assets under `host/direct`. Keep browser-specific
USB handling there rather than creating a second Python/.NET frontend.
The dashboards do not import or package each other's assets. Each owns its wireless form module and its Node tests;
matching firmware protocol behavior does not imply shared frontend code. Python/.NET build and run without `host/direct`,
and Direct can be served and tested without `host/web`, `host/python`, or `host/dotnet`.

### Shared API contract

[status-contract.json](../../host/web/tests/status-contract.json) lists the `GET /api/status` fields both native servers
must emit, the `nested` fields inside `memory` and `board_configuration`, the removed fields neither may emit, and
removed routes that must return 405 (such as `GET /api/wireless`); Python and .NET tests each check it. Staleness is
reported only through `telemetry_stale`, `storage_stale` and `self_test_stale`: a report is stale until first received
and after the [freshness limit](../reference/usb-hid.md#input-freshness), and the server nulls the stale section when
the snapshot is read, so a silent board ages out without a new report. Both servers nest USB-drive ownership (`msc`) and
`filesystem_format` in `memory`, and next-boot settings (`system_clock_mhz`, `sdcard_profile`, `radios`,
`tcp_cli_enabled`) in `board_configuration` beside the boot-time `stored_*` values; `normalizeStatus()` in
`host/web/js/dashboard.mjs` lifts them out for the view. Feature `12` wireless status arrives in the same snapshot as
`wireless` and `wireless_error`; the server polls it every 0.5 s during a scan or connection test and every 2 s
otherwise.

Every `/api` error body is `{"error": "<message>"}`, and both servers map failures the same way:

| Status | Meaning | Python | .NET |
|---|---|---|---|
| 400 | Rejected input, before any HID write | `InvalidRequest` | `ArgumentException`, `JsonException`, `FormatException`, bad binding |
| 404 | Unknown `board` ID | `LookupError` | `KeyNotFoundException` |
| 409 | Board state conflict or firmware refusal; retry after the state changes | `RuntimeError` | `InvalidOperationException`, `NotSupportedException` |
| 503 | HID or TCP I/O, timeout, disconnect or malformed board data | `OSError`, other `ValueError` | `IOException`, `TimeoutException`, `InvalidDataException`, `UnauthorizedAccessException`, `SocketException`, `OperationCanceledException` |
| 500 | Server fault; logged, and the message is not exposed | anything else | anything else |

Wi-Fi endpoints replace 503 transport detail with a fixed per-endpoint message, for example "Wireless provisioning
outcome is uncertain; command was not resent". Console connect reports "TCP console connection failed: <detail>" and
logs it as a warning. The board monitor never stops on a fault: it reports the error in the
snapshot, prefixed "Internal monitor error" for a server fault, and reconnects after 1 s. A stored `busy` control result
is history and does not block later commands; the per-device lock serializes them.

Each rule has one owner. Endpoints reject a malformed request with 400 whatever the board state; the HID client
guards firmware capabilities and report field lengths, and in Python also protocol ranges because the CLI calls it
directly (.NET has no CLI, so its endpoints are the only range check); the monitor checks only board state
(connection, storage ownership, self-test, pending reboot).

### Command ownership and cancellation

One command owns a board at a time. A scan holds it for up to 15 s and a connection test for up to 35 s, so an API
command waits at most 2 s for the board and then returns 409 "Board is busy with another command; retry when it
finishes"; .NET also stops waiting when the request is aborted. The monitor's own polling waits without a limit.

`POST /api/wireless/cancel` during a running test does not wait for the board. It flags the test, which sends opcode
`10` at its next 0.5 s poll, clears its in-flight uncertainty (the cancelled test never reports a result), and fails
with "Wi-Fi test cancelled"; the cancel request returns `{"cancelled": true}` at once. A cancel that arrives after the
test's last poll is sent when the test returns. With no test running, cancel sends opcode `10` directly. The browser
waits 20 s for a scan (2 s board wait plus the 15 s scan) and 40 s for a test.

Command replies share a top-level key: `msc` for `POST /api/msc` and `/api/msc/force-return`, `settings` for
`POST /api/settings`, `networks`, `test` and `cancelled` for the Wi-Fi scan, test and cancel routes. Python also returns
the firmware `control_result`, which .NET omits. LED, reboot and self-test (202) replies are acknowledgements; the
browser applies the requested state on success and reads the outcome from the next status snapshot.

Non-GET `/api` requests with a foreign `Origin` return 403. The web console long-polls
`GET /api/console`; each asynchronous request waits up to 2 s for board output, honors request cancellation, and does
not block input.

### Dashboard component isolation

Direct and the Python/.NET dashboard **must not share any dashboard components**. This includes HTML, CSS, JavaScript
modules, renderers, controls, transport adapters, assets, and test helpers. Do not introduce cross-dashboard imports,
symlinks, shared component directories, or build-time copying from one dashboard into the other. Similar behavior must
be implemented and maintained independently within each dashboard.

Python and .NET may continue sharing their own frontend in `host/web`; this does not include Direct. The firmware
[HID Report Reference](../reference/usb-hid.md) remains their common specification, not a shared
dashboard implementation. Repository CI may run both independent test suites.
The [dashboard isolation contracts](../../tests/contracts/test_dashboard_isolation.py) check local module/asset
boundaries and ensure the native hosts do not package or serve Direct components.

In the shared dashboard, active or pending self-tests disable board configuration and reboot controls without
adding an automatic test warning above Pin configuration. The Save button state indicates unsaved changes without
a separate notice. Explicit settings results/errors remain visible, and reboot confirmation still warns about
discarding unsaved changes; test progress belongs in the storage benchmark section.
Self-test controls stay pending throughout HTTP command delivery, even when idle telemetry sequences advance or
wrap. After delivery, wait for a newer self-test report before releasing the pending gate; an active report then
keeps conflicting controls disabled through cleanup.
USB sharing keeps the benchmark hidden and conflicting controls disabled from request submission until telemetry
confirms the requested owner with no transition pending, or reports rejection.
Intermediate disconnected or stale firmware-owned snapshots must not briefly restore the benchmark.
Enable USB drive opens the same styled confirmation pattern as reboot and return on both dashboard frontends.
The dialog explains exclusive host ownership, uninterrupted HID/CDC, unavailable self-test, confirmed forced saving, and safe eject.
Cancel, Escape, or disconnect sends no enable request; confirmation rechecks current storage safety gates.
Local settings edits remain available while shared. Supporting firmware permits **Save&Reboot** only through the
[forced-save confirmation](../reference/settings.md#forced-save-and-reboot); standalone reboot remains blocked.

Current host behavior follows the firmware's implemented HID reports. The
[HID Report Reference](../reference/usb-hid.md) is the authoritative protocol contract; host
changes must preserve its framing, validation, and asynchronous-operation semantics.

Only one active HID monitor per board should be used at a time. Separate host processes do not share device readers
and can conflict for access to the same USB interface.
