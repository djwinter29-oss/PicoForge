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

## Qualify the Python Host with Hardware

The Python monitor polls HID input reports every 50 ms while releasing the device lock between batches. Host tests
verify that commands can acquire the lock between polls, but they cannot establish real USB report latency, CPU use,
or device handoff behavior. Before changing this interval or claiming physical qualification, test the packaged or
source-installed Python host with representative RP2040 and RP2350 boards:

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
dotnet test host/dotnet/PicoForge.sln --configuration Release --nologo
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

## Shared Frontend and Protocol Boundaries

Python and .NET serve the same assets from `host/web`; keep shared UI behavior backend-neutral and preserve the common
API response contract. Direct is independent and owns its renderer and assets under `host/direct`. Keep browser-specific
USB handling there rather than creating a second Python/.NET frontend.
The dashboards do not import or package each other's assets. Each owns its wireless form module and its Node tests;
matching firmware protocol behavior does not imply shared frontend code. Python/.NET build and run without `host/direct`,
and Direct can be served and tested without `host/web`, `host/python`, or `host/dotnet`.

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
confirms the requested owner with no transition pending, or reports rejection, including with older firmware that reconnects.
Intermediate disconnected or old firmware-owned snapshots must not briefly restore the benchmark.
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
