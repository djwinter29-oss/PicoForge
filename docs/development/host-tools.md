# Host Tools Development

Python and .NET serve the shared dashboard in `host/web/src`. Direct lives independently in `host/direct` and connects
over WebHID or Web Bluetooth. For installation and everyday use, see the [Python](../../host/python/README.md),
[.NET](../../host/dotnet/PicoForge/README.md), and [Direct](../../host/direct/README.md) host guides.

Run commands below from the repository root.

- [Launch a host](#run-a-host-application)
- [Choose where to edit](#shared-dashboard-changes)
- [Run hardware-free tests](#run-host-tests)
- [Check architecture and ownership contracts](#shared-frontend-and-protocol-boundaries)
- [Measure performance](#performance-baselines)
- Qualify hardware: [Python](#qualify-the-python-host-with-hardware) or [.NET](#qualify-the-net-host-with-hardware)

## Run a Host Application

Install only the toolchain needed for your change:

| Work | Prerequisite |
|---|---|
| Python host and repository contracts | Python 3.10+ |
| .NET source host and tests | .NET 10 SDK |
| Shared and Direct frontend unit tests | Node.js with the built-in test runner |
| Direct USB use | WebHID-capable desktop browser and USB permission |

Choose one launcher:

```sh
tools/host/run-python.sh
tools/host/run-dotnet.sh
tools/host/run-direct.sh
```

Python and .NET default to <http://127.0.0.1:5000/>; run only one server on that port at a time. Direct defaults to
<http://localhost:8000/>. On Linux, HID access may require the
[permission setup script](../../tools/host/hid-permissions-setup.sh), followed by reconnecting the board.
Use only one active HID monitor per board: separate host processes do not coordinate device readers.

The Python launcher provisions its environment on first use. For an explicit source installation:

```sh
python3 -m pip install --editable host/python
```

See the [CDC CLI guide](../guides/console.md) for the USB console and
[Wi-Fi TCP terminal](../guides/console.md#wi-fi-tcp-terminal) for bridge setup, limits and security.

## Shared Dashboard Changes

Edit `host/web/src` for Python/.NET UI changes, not generated `.NET wwwroot` files.

| Location | Responsibility |
|---|---|
| `host/web/src/` | Deployable HTML, CSS, JavaScript and favicon |
| `host/web/tests/js/`, `css/`, `integration/` | Module, stylesheet and mounted-app checks |
| `host/web/tests/browser/` | Browser runner, checks and measurements |
| `host/web/tests/support/` | Shared-dashboard-only DOM and API fixtures |
| `host/web/tests/types/`, `host/web/tsconfig.json` | Strict JavaScript and negative type contracts |
| `host/web/tests/status-contract.json` | Common native-host status contract |
| `host/python/`, `host/dotnet/PicoForge/` | Native transport, monitoring and local HTTP APIs |
| `host/direct/` | Independent dashboard, bridge and tests |

Public asset routes remain `/`, `/console`, `/favicon.svg`, `/css/...` and `/js/...`; `src` is not a URL prefix.
Python source runs use `host/web/src`, while wheels package assets under `picoforge/web`. .NET build/publish copies
only source assets into `wwwroot`. Neither host ships frontend tests or type-check configuration.

### UI state and safety

Keep these invariants when changing shared controls:

- Each board owns its command state, drafts, wireless staging and recovery obligations. Selection and command
  generations fence asynchronous replies, including delayed response bodies and failures. Rendering does not reset
  drafts or initialize a new lifecycle from cached status.
- One operation policy drives admission, blocking explanations and confirmation handlers. Confirm again if the
  reviewed proposal, credential action, generated key revision or tested-pair identity changes. Confirmation identity
  must not contain secret bytes. Recheck admission immediately before sending.
- Validate lifecycle facts before accepting status or mutation replies. Missing or mistyped facts fail closed while
  retaining the last snapshot, drafts and recovery state; only explicit `connected: false` proves disconnection.
  Malformed acknowledgements are unresolved delivery, not permission to replay. Optional diagnostics may degrade to
  `--` with a warning without discarding valid lifecycle facts; distinguish unavailable, false and unassigned values.
- Benchmark-capable boards need fresh benchmark and storage facts for dependent writes. A periodic 8-bit benchmark
  counter is not a command identity and cannot settle unknown delivery. After confirmed completion, establish a fresh
  status baseline and wait for a later changed counter before releasing the report fence.
- Keep saved wireless configuration separate from drafts and temporary candidate SSIDs. Accept clean remote updates;
  preserve dirty drafts and block conflicting Save. Discard clears wireless edits only, after any required staging
  cleanup, and waits for a new accepted baseline. Clock/storage drafts remain intact.
- Credential intent is explicit: preserve, replace or erase. Control values alone never authorize erasure. Replacement
  requires successful, unexpired testing; failed/replaced tests invalidate earlier approval. A tested Save returns
  the host-issued `test_id` and firmware `test_generation` with `use_tested_wifi: true`. Native hosts verify current
  ownership, generation and exact staged fields under the device lock. Status/Re-sync cannot create test ownership.
  This identity is not authentication or an atomic comparison against independent external HID writers.
- Unrelated saves omit credential fields and refuse foreign staged fields. Radio enable edits require saved or valid
  replacement credentials. Missing/stale wireless facts block radio edits, not otherwise-safe clock/storage saves.
  Firmware staging flags remain authoritative; local progress and expiry use monotonic elapsed time.
- Cleanup failures preserve staging ownership. Known refusal permits explicit cleanup retry; unknown cancellation
  delivery requires read-only Re-sync first. Re-sync never replays the original command. If no matching terminal result
  exists, require the documented reconnect flow for that same board; unrelated results or boards cannot release it.
- Confirmed save/reboot blocks later writes until an observed reboot cycle or reconnect, even if readback validation
  subsequently fails. Storage handoff and benchmark gates persist across stale/intermediate observations.
  USB sharing requires confirmation and safe eject; forced save uses the
  [explicit data-loss policy](../reference/settings.md#forced-save-and-reboot). Standalone reboot remains blocked
  while USB owns storage.

The console validates mutation acknowledgements and never replays input after an uncertain response. It coalesces
output into one animation frame, retains bounded per-line DOM nodes, preserves scroll position when reading history,
and cancels pending rendering on navigation. Keep native copy line breaks and keyboard focus behavior intact.

## Run Host Tests

### Choose the smallest check

For the complete hardware-free host wrapper:

```sh
python3 -m pip install --editable host/python -r host/python/tests/requirements.txt
tools/validation/test-host.sh
```

The wrapper checks the Python lock and focused strict types, then Python, independent Direct/shared frontend and .NET
tests. For a focused change, choose the relevant command:

```sh
python3 -m unittest discover -s host/python/tests -v
python3 -m uv lock --check --project host/python
python3 -m pyright --project host/python/pyproject.toml
node host/direct/tests/run-tests.mjs
node host/web/tests/run-tests.mjs
npx --yes --package typescript@5.9.3 tsc --project host/web/tsconfig.json
dotnet test host/dotnet/PicoForge.sln --configuration Release --nologo --filter "Category!=Performance"
```

Python and .NET tests cover protocol, API, lifecycle and ownership using fake transports, not physical USB devices.
Shared frontend tests cover mounted interactions and boundary validation; Direct owns a separate suite.
Prefer production-shaped replies and deterministic event/clock control over sleeps. Keep strict type contracts at
API, command-state and draft boundaries; runtime validation remains necessary at peer boundaries.

For repository contracts, including documentation links:

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
# Documentation-only change:
python3 -m pytest tests/contracts/test_documentation_links.py
```

See the [test ownership index](../../tests/README.md) for cross-project prerequisites.

### Shared browser checks

Install browser prerequisites when setting up this suite, then run:

```sh
python3 -m pip install -r host/web/tests/browser/requirements.txt
python3 -m playwright install --with-deps chromium firefox webkit
python3 host/web/tests/browser/run.py --browser chromium firefox webkit
```

The runner also needs Python host dependencies and the .NET 10 SDK. It builds .NET and serves the actual Python/.NET
templates, assets and response headers with mocked APIs; it sends no hardware requests. Chromium is the default;
requested missing engines fail explicitly. Linux CI runs both hosts through all three engines.

Coverage includes command/recovery gates, wireless draft and staging flows, confirmation renewal, diagnostic rendering,
native keyboard/focus interactions, reflow, clipboard refusal and console rendering/scroll behavior. CSS zoom and a
320-pixel viewport are automated layout checks, not native browser-zoom or screen-reader certification.
For a static-only manual check:

```sh
python3 -m http.server 8000 --directory host/web
# Open http://127.0.0.1:8000/tests/browser/index.html
```

Before release, record manual screen-reader checks on supported desktop platforms, native 200/400-percent browser zoom,
and physical provisioning/storage/reconnect checks. Record platform, reader/browser, board, firmware, steps and results.
Mocked browser tests do not complete these qualifications.

### Python wheel check

CI and release packaging use the installed-wheel check with an isolated interpreter:

```sh
python -m pip wheel --no-deps host/python --wheel-dir build/wheels
python -m venv build/wheel-check
build/wheel-check/bin/python -m pip install build/wheels/*.whl
build/wheel-check/bin/python -I tools/validation/check-python-wheel.py 0.0.0.dev0
```

Replace the expected version for stamped releases; on Windows use `Scripts` instead of `bin`. The check rejects source
imports, compares the full installed asset payload, serves routes without hardware, and exercises imports and CLI entry
points. .NET release smoke checks similarly run the extracted executable and compare its full dashboard payload.

## Shared Frontend and Protocol Boundaries

The [HID Report Reference](../reference/usb-hid.md) owns framing, validation, freshness and asynchronous-operation
semantics. Keep shared UI behavior backend-neutral; browser-specific USB handling belongs in Direct.

### Shared API contract

[status-contract.json](../../host/web/tests/status-contract.json) defines required status fields, nested
`memory`/`board_configuration` fields, removed fields and removed routes; both native hosts test it.
`GET /api/boards` discovers boards; status and commands address one using `?board=<id>`.
`GET /api/status` supplies identity, settings, telemetry, storage, self-test and wireless observations.

- `memory` owns `msc` and `filesystem_format`; `board_configuration` owns next-boot settings and boot-time `stored_*`
  values. The shared UI normalizes this shape for presentation. Python CLI `picoforge status` alone retains deprecated
  top-level `msc` for compatibility.
- `telemetry_stale`, `storage_stale` and `self_test_stale` apply on snapshot read and null expired sections, including
  before the first report. Wireless uses `wireless`/`wireless_error`; successful reads record monotonic receipt time.
- Endpoints validate malformed input before board lookup. HID clients guard capabilities and field lengths; Python's
  client also checks protocol ranges for CLI callers. Monitors own board-state admission.
- API bodies are capped at 4 KiB, console bodies at 2 KiB. Settings and Wi-Fi test bodies reject unknown fields;
  an open-network test password may be absent or empty, not null. Non-GET requests with foreign `Origin` return 403.

Every API error includes `{"error": "<message>"}`:

| Status | Meaning |
|---|---|
| 400 | Invalid input, before HID writes |
| 404 | Unknown board ID |
| 409 | State conflict, firmware refusal, quarantine, retirement or capacity conflict |
| 413 | Request exceeds the body limit |
| 503 | Transport failure, timeout, disconnect or malformed board data |
| 500 | Logged server fault; internal message is not exposed |

Wi-Fi transport failures use fixed per-endpoint uncertainty messages. Console connection errors include transport
detail and are logged as warnings. Expected configuration-read failures preserve identity/input diagnostics with
`settings_supported: false` and `settings_message`; configuration-dependent writes stop, while LED and read-only
Re-sync remain available. Unexpected parser/programming faults are internal errors, not degraded connected success.

### Command ownership and cancellation

One command owns a board at a time. API commands wait at most 2 s for ownership before returning 409; .NET also honors
request abortion while waiting. Polling waits without a limit. Native I/O already accepted is not interrupted by
request cancellation, and native calls must return before shutdown or software deadlines can finish.

Scans hold ownership for up to 15 s, connection tests up to 35 s. The browser allows 20 s and 40 s respectively.
During a running test, `POST /api/wireless/cancel` flags cancellation without waiting and immediately returns
`{"cancelled": true}`. The test sends opcode 10 on its next 0.5 s poll, or when returning if cancellation arrived after
the last poll, then reports HTTP 409 "Wi-Fi test cancelled". With no test running, cancel sends opcode 10 directly.
Acknowledging the cancellation request is not proof that firmware cleanup completed.

Uncertain writes retain their exact transaction reference across reconnects. Read-only Re-sync matches that reference
without replay. Known cancellation failure instead retains `wireless_cleanup_required`; unrelated writes and console
creation stay blocked until explicit cleanup succeeds. Unknown cancel delivery owns the cancel reference, not the
cancelled test generation. Cleanup and uncertainty prevent monitor retirement.

Mutation response keys are `msc` (storage), `settings`, `networks`, `test` and `cancelled`; Python additionally emits
`control_result`. LED/reboot acknowledgements and self-test HTTP 202 confirm delivery, with status supplying subsequent
observations. A historical `busy` result alone is not an active command gate.

### Native monitor ownership

Both registries serialize scans and publish complete discovery results. Duplicate identities are omitted with warnings
and quarantined immediately, including writes through retained references; unrelated boards remain usable and accepted
commands may finish. Only positive unique discovery clears retained identity quarantine, not absence or failed scans.
Read-only Re-sync does not clear quarantine.

Retirement first claims idle command ownership without waiting. Active commands postpone it; unresolved outcomes pin
the monitor. A claim rejects new commands, and the identity/capacity slot remains owned until cleanup completes.
Each registry retains at most 16 active, retiring or cleanup-failed entries. Shutdown joins existing cleanup instead
of starting overlapping disposal.

Every opened handle, including rejected reconnect candidates, has an owner before initialization/validation.
Native close failure is terminal: retain the failed owner, stop reconnects, report cleanup failure and require restart;
never retry close or open a replacement. Confirmed save/reboot effects are published through one result path, including
Re-sync, even if subsequent readback validation fails. No payload secrets belong in retained result metadata.

Console creation requires connected, fresh, error-free wireless status without cleanup pending. Only one active/opening
session is allowed. Setup has a 3 s timeout; authentication shares an 8 s budget including a 3 s challenge deadline,
USB proof and AUTH sending. Shutdown closes opening and active sockets and forbids late proof continuations from sending
AUTH or publishing sessions. An already accepted native proof may finish. Console long-polls wait up to 2 s without
blocking input; genuine polling failure closes the session.

### Python host design

Report codecs are pure functions grouped by report family; decoded models live in `picoforge.reports.models`, with
compatibility exports in `picoforge.hid.models`. Annotation-only imports do not initialize HID transport.
The HID client owns one handle and a reentrant transaction lock for reads, writes, wireless lifecycle and close.
Close waits for accepted I/O, closes once and rejects queued work. The monitor owns reconnects and snapshots;
routes validate/serialize without mutating internal snapshots. CLI entry points share one implementation.

Client installation/removal, connected/disconnected publication and LED result publication occur under command
ownership, so startup or older requests cannot overwrite newer accepted state. Serial boards absent from successful
discovery expire after 30 s; path-only boards after 5 s, subject to ownership/cleanup retention.

Input polling is 50 ms while browsers request status, slowing to 500 ms after 5 s without requests; the device lock
is released between batches. Wireless polling is 500 ms during scan/test, 2 s normally and 5 s while idle.
Wireless expires at 6 s without a successful refresh in both native hosts; failed reads do not extend freshness.
Console idle timers are cancelled on disconnect/shutdown; a concurrent console close returns 409, an idle poll succeeds.

Keep runtime pins and `uv.lock` synchronized with `python3 -m uv lock --project host/python`.
Contributor validators belong in `host/python/tests/requirements.txt`, not the shipped package.

### .NET host lifecycle

The registry owns asynchronous disposal, including dynamically started and retired monitors. Monitoring starts
asynchronously on first access, so initial status may briefly wait for HID identity/configuration. Initial setup and
forced-save reconnect follow the [native ownership rules](#native-monitor-ownership); cleanup failure retains both the
owner and relevant initialization error rather than retrying.

Polling/freshness limits are named constants, not runtime tuning options:

| Report | Poll interval | Expires at |
|---|---|---|
| Input | 100 ms | Telemetry 750 ms; storage/self-test 1500 ms |
| Wireless | 500 ms during scan/test, otherwise 2 s | 6 s |

Wireless retains its last successful report for diagnostics, but failed/expired reads set `wireless_error` and prevent
new console sessions. Successful polling refreshes it. Configuration is read per connection, not retried every poll.
The executable and integration tests use the same `DashboardApplication` composition for headers, binding, limits,
error mapping, templates and routes. Fake-transport lifecycle tests and extracted-package smoke checks do not replace
[hardware qualification](#qualify-the-net-host-with-hardware).

### Direct dashboard independence

Direct is an independent product. It **must not share any code** with the Python/.NET dashboard: not HTML, CSS,
JavaScript, Python, renderers, controls, protocol codecs, transport adapters, the terminal bridge, assets, tests, or
test helpers. Do not introduce cross-dashboard imports, symlinks, shared component directories, build-time copying,
or copy-paste forks kept in sync with `host/web`, `host/python`, or `host/dotnet`. Similar behavior must be designed,
implemented, and maintained independently in Direct; behavioral changes do not require matching changes to the other.
The sole asset exception is the branding favicon: keep the two separately hosted `favicon.svg` files byte-identical,
without introducing build-time copying or a runtime dependency.

Direct code and its Python-standard-library bridge live in `host/direct`. The native hosts build/run without Direct;
Direct serves/tests without `host/web`, `host/python` or `host/dotnet`. Firmware HID/BLE specifications are shared
contracts, not shared implementations. CI may run both independent suites.
[Isolation contracts](../../tests/contracts/test_dashboard_isolation.py) check imports, packaging and byte-identical
copies other than the favicon; near-copies remain a review concern.

## Performance Baselines

These are dated local observations, not hardware qualification or universal timing guarantees. Measure before changing
polling, snapshot copying or ownership boundaries; fewer wakeups alone do not establish lower CPU use.

### Python dashboard performance baseline

```sh
host/python/.venv/bin/python tools/validation/bench-python-dashboard.py
# Read-only physical-board mode; stop other dashboards first:
host/python/.venv/bin/python tools/validation/bench-python-dashboard.py --board --serial SERIAL
```

The hardware-free workload decodes representative reports and refreshes receipts so freshness filtering cannot remove
measured sections. Snapshot timing includes that refresh; Flask timing excludes network transport. The mocked worker
CPU sample requires a live, connected, error-free worker and measures idle 500 ms polling, not active 50 ms polling.
Board mode closes the monitor before exclusive wireless-read measurements.

| Path | Baseline: 2026-11-06, x86-64 Linux, hardware-free | Revisit when |
|---|---|---|
| Snapshot deep copy and receipt refresh | ~40 us/request | One request exceeds 1 ms |
| Flask `GET /api/status` | ~0.5 ms/request; typically 1/s/tab | Many tabs or noticeable latency |
| 500 ms idle poll | ~0.5 ms CPU/s/board with mocked HID | Idle CPU exceeds ~1%/board |

Time save, scan and connection-test lock holds on hardware before splitting ownership; preserve the
[write-safety contract](#command-ownership-and-cancellation).

### .NET dashboard performance baseline

```sh
dotnet test host/dotnet/PicoForge.sln -c Release --filter Category=Performance --logger "console;verbosity=detailed"
```

`PerformanceTests` uses a mocked board, fixed clock, production JSON/composition and payload-shape assertions.
The default host suite excludes this category; its failure ceilings are deliberately generous.

| Path | Baseline: 2026-11-06, x86-64 Linux, Release | Revisit when |
|---|---|---|
| Freshness-filtered `DeviceMonitor.Snapshot` | 0.29 us/read | One read exceeds 50 us |
| Status JSON serialization | 19.0 us/status | One status exceeds 0.5 ms |
| Kestrel `GET /api/status` | 0.26 ms/request; typically 1/s/tab | Many tabs or noticeable latency |

HID commands occupy one thread-pool thread under the device lock for up to 35 s. This localhost-dashboard tradeoff
should change to dedicated I/O only if concurrent requests start queueing.

### Console rendering baseline

Run the [shared browser suite](#shared-browser-checks) to measure the actual console module/styles at 1,000 lines,
512 columns and 30 six-chunk bursts.

| Historical Chromium observation through both hosts | Incremental rendering | Earlier full-transcript rendering |
|---|---|---|
| Cursor text-construction p95 | ~0.1 ms | ~19–29 ms |
| Layout p95 | ~3–4.3 ms | ~80–156 ms |

Baseline retained as of 2026-11-26; the original run date/environment details were not recorded. These numbers are
context, not thresholds. The runner enforces one render/burst, at most 24 mutation records for a six-line burst,
retained row identity and scroll anchoring; all three engines check copied line separators.

## Qualify the Python Host with Hardware

Host tests cannot establish real USB latency, CPU use or device handoff. Use representative RP2040/RP2350 boards:

1. Record OS, Python version, firmware revision, board and storage/radio configuration. Launch the host and leave Live
   diagnostics visible for at least 5 minutes; confirm runtime/storage/self-test reports remain current.
2. Perform at least 10 USB detach/reconnect cycles; check selection, current telemetry and absence of duplicate workers.
   Keep an unrelated board connected if multi-board use is in scope.
3. Use a disposable/backed-up SD card. Enable USB storage, check firmware-owned controls are blocked during handoff,
   safely eject and return ownership. Use force-return only with no valuable data and after reviewing its warning.
4. On a W board and test access point, scan, test, cancel/clear and check completion/timeout recovery. Avoid production
   credentials.
5. Save disposable settings and reboot; check `reboot_pending` blocks writes until reconnection. Repeat after read-only
   Re-sync confirms uncertain save/reboot, without resending. Refused/failed saves must not invent a reboot gate.
6. Shut down while idle, testing Wi-Fi and authenticating TCP. Opening sockets must close; no late AUTH/session may
   publish. Restart and reconnect normally. Already accepted HID operations may finish.
7. If duplicate-serial boards are available, rescan with both attached: quarantine must block retained references while
   leaving other boards usable. Disconnect both and rescan within the 30 s grace period; absence must not clear quarantine.
   Reconnect one uniquely and confirm monitor reuse without replay.
8. Record worst report/UI latency, process CPU, reconnect results and errors. Failed steps remain qualification gaps.

Store dated observations in [validation records](../validation/records/README.md) and outstanding coverage in
[qualification gaps](../validation/qualification-gaps.md).

## Qualify the .NET Host with Hardware

The 100 ms input poll observes firmware telemetry/storage/self-test emissions every 250/500/500 ms. Unit tests do not
establish real freshness, multi-board CPU or storage durability. Use representative RP2040/RP2350 boards:

1. Record OS, SDK/runtime, firmware, board and storage/radio configuration. Launch the host and leave Live diagnostics
   visible for at least 5 minutes; check reports remain current.
2. Perform at least 10 USB detach/reconnect cycles without stale/duplicate monitors. With multiple boards, verify an
   unrelated board stays responsive during scans and initial monitor startup.
3. Use a disposable/backed-up SD card. Enable USB storage, safely eject, return ownership and wait for fresh mounted
   status. Exercise [forced-save recovery](../reference/settings.md#forced-save-and-reboot) only without valuable data.
4. On a W board/test access point, scan/test/cancel during joining and as completion arrives. Check cleanup without
   automatic resend. Failed cleanup must block writes/console across reconnect until explicit cleanup succeeds;
   unknown delivery requires read-only Re-sync first. Avoid production credentials.
5. Measure first status-response latency, time to current connected telemetry, worst report/UI delay, command latency,
   idle/multi-board CPU and reconnect results. Compare 10/50/100 ms polling only with otherwise equivalent builds,
   firmware, board, workloads and observation periods. Wakeup counts alone do not establish CPU reduction.

Store dated observations in [validation records](../validation/records/README.md) and outstanding coverage in
[qualification gaps](../validation/qualification-gaps.md). Failed checks are not qualified by passing unit tests.
