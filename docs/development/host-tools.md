# Host Tools Development

PicoForge has Python and .NET dashboards that share the frontend in `host/web/src`, plus the independent Direct dashboard
in `host/direct`, which connects over USB (WebHID) or Bluetooth LE (Web Bluetooth). For installation and everyday use,
see the [Python host guide](../../host/python/README.md), [.NET host guide](../../host/dotnet/PicoForge/README.md),
and [Direct dashboard guide](../../host/direct/README.md).

Run commands from the repository root. Host unit tests do not require a board; physical device checks require an
identified board and appropriate USB permissions.

## Prerequisites

- Python 3.10+ for the Python host and repository contracts
- .NET 10 SDK for the .NET host and tests
- Node.js with the built-in test runner for shared and Direct dashboard tests
- A WebHID-capable desktop browser for physical browser use

Install only the toolchain needed for the host or checks you are changing.

## Shared Dashboard Changes

Edit `host/web/src` for both Python and .NET; do not edit generated `.NET wwwroot` copies. The shared and Direct dashboards
remain separate implementations, with byte-identical favicons for consistent branding.

The shared dashboard keeps deployable assets separate from tests and tooling:

```text
host/web/
  src/
    index.html, console.html, favicon.svg
    css/
    js/
  tests/
    index.test.mjs, favicon.test.mjs
    css/          # stylesheet contracts
    js/           # unit tests named after their source module
    integration/  # mounted app lifecycle and backend interaction
    browser/      # Chromium checks, measurements, runner, and requirements
    support/      # shared-dashboard-only DOM and API fixtures
    types/        # negative type contracts
    status-contract.json
    run-tests.mjs
  tsconfig.json
```

Public routes remain `/`, `/console`, `/favicon.svg`, `/css/...`, and `/js/...`; `src` is a repository layout, not a URL
prefix. Python source imports use `host/web/src`; wheel assets remain under `picoforge/web`. .NET copies only the source
assets into `wwwroot`. Neither host packages the test harness or type-check configuration.

Wireless drafts require successful, unexpired Wi-Fi testing before Save. Failed replacement tests invalidate the
previous test; clearing temporary credentials must complete before other board operations resume. Control-key copy
feedback is fenced to the current board lifecycle and key draft, so an earlier clipboard operation cannot claim that
a replacement key was copied. Failed saves retain their error feedback; regenerate the key or disable Bluetooth
before retrying. USB-shared storage blocks wireless Save and explains how to return ownership safely.
Re-sync locks board selection and discards continuations from a previous board generation. Cancellation delivery
timeouts and network failures retain uncertain outcomes; known refusals remain retryable after explicit cleanup.
Disconnected polling clears drafts once per lifecycle transition, not on every status render.
When either host reports `wireless_cleanup_required`, unrelated writes remain blocked and the status action offers
**Clear Wi-Fi test**. Known cleanup failures permit only an explicit cleanup retry. Unknown cancellation delivery
requires **Re-sync** first; a confirmed completed cancel clears local recovery state without resending it.
Scan, test, and cancellation response failures remain owned by the originating board, even after disconnect and
selection changes. Local response uncertainty uses the same recovery admission as server uncertainty. Re-sync remains
available inside the Wi-Fi dialog as well as in the status footer; malformed recovery replies cannot release the gate.
Without firmware reconciliation support, persistent feedback directs the user to reconnect and reload.

The pure operation policy combines reported facts and local activity for command admission and rendering. Benchmark
size edits use the complete view projection, never a raw status render that could discard local gates. Local delivery,
uncertainty, and reboot lifecycle state remain separate from server-reported pending flags and credential drafts.
The application passes one operation result to the presentation helpers; settings, recovery, storage and benchmark
controls consume that result rather than reconstructing separate admission policies.
Each board has one authoritative command-state object; selection changes switch references instead of copying fields
between selected-board globals and a map. Status-driven lifecycle transitions are strictly typed.
Local uncertainty records a typed recovery reason and matching evidence rather than an opaque callback. Wireless
controls initialize synchronously from the first ready status snapshot per board lifecycle. Later fresh snapshots
refresh clean wireless configuration, including TCP CLI and the lossless SSID. Live link state and scan progress do
not invalidate drafts. If the reported wireless configuration changes while local edits or tested credentials are
pending, the dashboard preserves those edits and blocks Save. **Discard wireless edits and reload** clears only the
wireless draft and secrets, then waits for accepted wireless configuration; clock/storage edits remain intact.
Any temporary pair must be cleared successfully first. Cleanup refusals retain the draft and open Wi-Fi settings for
an explicit retry; unknown delivery requires Re-sync, never automatic replay. An observation started before discard
cannot initialize the replacement baseline.
Feature 12 reports a temporary candidate SSID during Wi-Fi testing; it is not proof that the saved network changed.
The form keeps its saved baseline separate from local tests and observed staging. Temporary candidate SSIDs never
become a saved baseline, including when another client staged them before the first ready poll. Radio, TCP CLI, and
saved-key flags still participate in conflict detection. Manual SSIDs are compared as UTF-8 bytes; scan-selected SSIDs
retain their opaque bytes. Clock, storage and each radio have independent draft values. User change events record
those values; accepted status supplies the baseline for untouched fields. Save requests and confirmations use the
baseline plus drafts, never values scraped back from controls. The accepted-status initializer projects those values
into the controls; a clock or storage draft cannot freeze radios against a newer baseline.
Enabled/Disabled selectors contain only values. Separate **Configure...** buttons open the Wi-Fi and Bluetooth dialogs
without changing radio state or credential intent, even if a remote update arrives while a selector retains focus.
The wireless form owns radio drafts, network inputs, generated key and TCP switch. It projects explicit
**preserve**, **replace** or **erase** credential actions, shown in Save confirmation. A DOM value mismatch alone never
authorizes erasure. Untouched radio bits come from accepted saved settings, not stale controls.
Missing, stale or failed wireless status disables radio editing and blocks radio changes in both the view and Save
handler. Clock/storage-only saves remain available when otherwise safe and preserve the saved radio bits. Disabling a
configured radio after fresh wireless status arrives explicitly erases only that radio's credentials through the
combined provisioning commit. Form initialization consumes accepted status polls, not local re-renders of cached
snapshots; rendering does not reset lifecycle state or synchronize editable values.
A local tested pair requires matching SSID bytes, test generation and tested/staged-pair flags when a subsequent
observation arrives. An old or replaced candidate produces a recoverable waiting gate, not a sticky saved-configuration
conflict; matching observations release it, or the user can retest or clear staging. Foreign staging requires explicit
cleanup in Wi-Fi settings before Save, preserving unrelated local edits. Failed joins also require retesting or cleanup,
but never authorize Save or become permanent saved-configuration conflicts.
Clearing the only tested network also removes its dependent Wi-Fi enable draft when no saved network exists.
Explicit disables and enable drafts backed by saved credentials remain intact. Serialization and final Save admission
reject radio enable edits without saved or valid replacement credentials; a disabled select option is not the gate.

Both host monitors invalidate cached wireless observations when testing starts and when staging is cleared. A new
HTTP status request therefore cannot return a pre-test or pre-cleanup candidate as fresh evidence. The regular monitor
read repopulates status; no extra post-success HID read can turn confirmed completion into an uncertain result.
Successful test replies include a host-issued opaque `test_id` (32 lowercase hexadecimal characters) and the firmware
`test_generation` (1..255). A tested Save must return both with `use_tested_wifi: true`. Under device ownership, both
hosts require the acknowledged identity to match their current successful test and the live firmware generation, and
recheck the generation and exact staged-field mask before committing. Replaced tests, generation reuse and reopened
device handles cannot authorize an older browser's Save. Status/Re-sync alone never manufactures test ownership;
retest or clean up when the successful test response was lost. This is a version identity, not authentication, and does
not claim an atomic firmware-side comparison against independent external HID writers.
Unrelated saves omit SSID/password fields; only tested-pair replacement or explicit credential erasure sends them.
Both hosts refuse omitted, pre-existing staged fields rather than silently including another client's credentials.
Normal acknowledgements and Re-sync share typed terminal-result publication. Confirmed failed, busy, invalid, or
unsupported benchmark/storage results release only their matching local pending marker; their failure reason remains
visible across polling and board selection until an explicit retry. Successful acknowledgements still wait for fresh
ownership or benchmark-sequence evidence, and successful save/reboot results retain their reboot obligation.
The benchmark sequence is a periodic 8-bit report counter, not a run or command identifier. Advancing or wrapping it
can satisfy a fresh-report fence after confirmed delivery, but never settles unknown command delivery. That uncertainty
retains its own command identity until a terminal Re-sync result or the explicit no-result reconnect flow resolves it.
Confirmed benchmark completion rearms the report fence at confirmation time, even if reports arrived while uncertain.
The first connected, non-stale benchmark snapshot requested after completion establishes the observation baseline;
a subsequent changed report counter releases the fence. This also handles a cached report captured before completion
without treating it as post-command progress. This conservative fence can add an extra status-poll interval.
Status requests are fenced by both board-selection and command generations. A command starting or finishing
invalidates earlier in-flight status responses, including delayed JSON bodies and errors. Polling during wireless
testing can still display live progress, but delayed responses cannot overwrite post-command state.
Status replies validate connection, uncertainty, reboot and freshness booleans, plus available command-admission facts,
before normalization or any lifecycle transition. A malformed successful reply follows the status-read error path:
the last known snapshot, credential drafts and recovery obligations are retained, the error is displayed, and writes
remain blocked until a valid read. Only an explicit `connected: false` is disconnect evidence; missing or falsy
connection fields cannot settle unknown reboot delivery. Optional diagnostic additions remain compatible.
Malformed or non-terminal reconciliation results cannot release local gates. No recovery path automatically replays
the original command.
Browser-only benchmark and storage delivery failures also expose Re-sync. If the host has no matching result
(including an unrelated terminal result), writes stay blocked and the dashboard asks for a disconnect/reconnect of
that same board. Only a subsequently connected, non-stale status with benchmark-sequence or storage-ownership facts
releases the local pending marker for an explicit retry. HTTP errors, another board's reconnect, and repeated clean-host
replies do not satisfy this requirement. Re-sync never interprets a missing result as proof of non-delivery.
Acknowledged saves and reboots immediately block subsequent writes; old connected snapshots cannot clear this
per-board gate. It releases after an observed reboot-pending cycle or disconnect/reconnect, including when Re-sync
confirms completion. Successful save readback mismatches retain both the uncertainty and the confirmed reboot effect.
Unknown-board HTTP 404 responses trigger a board rescan at most once every five seconds.
The same capability-aware policy gates storage handoff, configuration save, reboot and wireless operations. On
benchmark-capable boards, a missing or stale benchmark report means activity is unknown, not idle. Storage-dependent
writes wait for fresh facts; read-only status/Re-sync and unrelated LED access remain available.
The policy also projects the blocking reason. One Save eligibility result and reason drive the button, tooltip,
settings note and confirmation handler. Confirmation rechecks current admission, storage ownership and the reviewed
proposal before sending anything. A changed clock/storage/radio proposal, credential action, generated key revision or
tested-pair identity requires renewed confirmation; the outgoing request cannot silently differ from the review.
The confirmation identity contains no password or generated key bytes. Repeated observations alone do not invalidate
an unchanged proposal. Ordinary boot settings retain the explicit forced-save path while
credentials require firmware-owned storage. Storage and settings notes explain missing/stale ownership or benchmark
status and pending transitions instead of leaving disabled controls unexplained.

Shared frontend regressions use independent mounted scenarios, production-shaped replies, distinct per-board status
objects, and an additive-listener DOM fixture. Wireless faults, gates, saved configuration and temporary staging also
belong to each board; late replies retain their originating board and request. Default Wi-Fi/Bluetooth observations
match the saved radio bits, and successful combined saves assert that invariant. Deliberately contradictory reports
require explicit fault overrides. Cancellation clears only that board's temporary staging.
All production JavaScript modules, including dashboard presentation,
status normalization and console lifecycle orchestration, have strict JavaScript type checks. Negative type contracts
cover the API and normalized view boundaries, per-board command state, status-driven transitions, reconnect recovery
reasons, radio drafts, credential actions and Save admission. Shared status types live at the existing API boundary;
no separate schema library or state-management dependency is required.
Mutation success bodies are validated against the operation's Python/.NET contract. Invalid JSON, malformed
acknowledgements, and contradictory readback are unresolved outcomes, not permission to replay. Explicit known HTTP
refusals remain distinguishable from unknown delivery.
Board discovery and scan entries are validated before rendering, including board identity, raw SSID bytes, signal
strength, and security field types. Combined Save validates credential-specific readback without treating omitted SSID
fields as permission to replace Wi-Fi credentials.
Optional diagnostic fields are checked separately from lifecycle facts. Missing manufacturer details display `--`;
malformed diagnostic values are removed with an explicit status warning, without discarding valid ownership facts,
drafts or command capabilities. Lifecycle validation remains fail-closed. Diagnostic normalization does not mutate
the response, and unknown additional fields remain compatible. Rendering distinguishes unavailable facts (`--`),
explicit `false` (for example, a closed CLI port), and the reported unassigned GPIO sentinel (`255`). Missing UART
indices do not produce a numbered heading, and missing reset causes do not masquerade as reported `Unknown` values.
CID dates require years 2000–2255 and months 1–12; manufacturer IDs, GPIOs and reset causes require integer byte values,
and UART indices must be 0, 1 or 255. Native hosts remain responsible for MCU/board-specific GPIO validity.
Benchmark labels follow the native hosts' state vocabulary, and missing-card diagnostics use reported card-detect
wiring rather than a fixed board GPIO.
Wi-Fi test progress and staged-credential countdowns use monotonic elapsed time; wall-clock changes neither expire a
valid stage early nor extend it. Firmware credential expiry remains authoritative.

```sh
node host/web/tests/run-tests.mjs
npx --yes --package typescript@5.9.3 tsc --project host/web/tsconfig.json
python3 -m pip install -r host/web/tests/browser/requirements.txt
python3 -m playwright install --with-deps chromium firefox webkit
python3 host/web/tests/browser/run.py --browser chromium firefox webkit
```

The browser runner requires the Python host dependencies and .NET 10 SDK. It builds the .NET host and serves the actual
Python/.NET templates and assets with their production response headers; API replies are mocked, so no hardware
requests are made. It defaults to Chromium; `--browser` selects one or more engines, and missing engines fail explicitly.
Linux CI runs the complete suite through both hosts in Chromium, Firefox and WebKit.
Both hosts run the same reboot and uncertainty gates, save-readback mismatch, 404 recovery, focus,
narrow-layout, network-selection, cancellation, browser-only uncertainty recovery, failed terminal-result recovery,
no-result reconnect recovery, periodic report-counter wraparound, delayed pre-command status, stale benchmark admission,
clean wireless-baseline refresh, dirty-draft conflicts, new-network and replacement-test candidate polls through
Save&Reboot, cached previous-candidate recovery, same-SSID cross-client test replacement, foreign-stage cleanup and
credential omission on key-only Save, clock/storage drafts with remote radio changes, missing first wireless status,
forced-handler readiness enforcement, explicit credential-erasure recovery, malformed-status draft retention,
reboot-recovery fencing, and clipboard checks. Four additional native-interaction scenarios per host cover both radios
after remote enable/disable, keyboard activation and Escape/focus restoration, repeated-status draft invariance,
explicit credential erasure and DOM-only changes that must not alter saved drafts. Native selects emit their actual
input/change sequence; disabling a tested pair clears staging once without reverting the requested disable.
Three further native scenarios per host cover renewed confirmation after a remote proposal change, clearing the only
tested network, and local conflict discard with cleanup retry while preserving clock/storage drafts. Focused sequence
tests also cover credential replacement during confirmation, missing credential prerequisites, repeated observations,
and known/unknown cleanup failures.
Each engine/host combination also traverses clock editing, Save confirmation and Bluetooth configuration with native
Tab/Shift+Tab and Enter, without programmatic focus jumps. Checks verify visible, in-viewport, unobscured focus,
initial Cancel focus and Escape/focus restoration without sending a command. Repeated dialog keypresses must leave
exactly one mounted confirmation-close listener; mounted regressions also cover all four confirmation dialogs.
Boundary-to-render browser checks verify unavailable, explicit-false and unassigned diagnostic states while retaining
settings drafts and valid command admission.
Reflow checks exercise CSS zoom at 200 percent, a 320-pixel viewport, dialog reflow and explicit clipboard-refusal
feedback without reporting success. CSS zoom is not native browser-zoom certification, and these automated checks do
not certify screen-reader usability.

Before release, perform and record these manual qualification checks:

- With a screen reader on each supported desktop platform, verify named settings and dialog controls, disabled-action
  explanations, status/error announcements and focus restoration. Record the reader, browser, platform and results.
- Using native browser zoom, check 200-percent and 400-percent zoom for readable content, reachable controls and visible
  focus; distinguish necessary pin-table/console scrolling from page-wide overflow.
- On physical boards, qualify provisioning, credential expiry, storage ownership transitions and disconnect/reconnect
  recovery with representative traffic. Record board, firmware, host, steps and results.

Screen-reader, native browser-zoom and physical-board qualification remain manual and are not established by these
mocked browser tests. The current automated coverage does not justify marking any of those manual checks complete.
Wireless test mocks change the polled SSID and staged-pair generation like the firmware, rather than continuing to
report the saved SSID after a successful test. Runtime boundary tests also reject missing or mistyped lifecycle facts
and test identities. These are protocol-shaped simulations, not physical provisioning qualification.
For a static-only manual check, serve `host/web` locally and open `/tests/browser/index.html`.

Console success bodies are validated by operation; malformed acknowledgements close the session without replaying
input. Output is coalesced into one pending animation frame; input is sent immediately, disconnect flushes final
output, and page navigation cancels pending output. The console retains bounded per-line DOM nodes, removes only
expired scrollback rows, and updates changed lines/cursor segments without rebuilding the transcript. Output follows
the bottom only when the reader is already there; new output and scrollback trimming preserve the viewed content
when reading older lines.
The runner measures parsing, cursor text construction, row appends, and layout using the actual console module and styles
at 1,000 lines of the full 512-column bound, over 30 six-chunk bursts. Local Chromium measurements through both hosts
kept one render per burst, with cursor text-construction p95 at about 0.1 ms and layout p95 about 3–4.3 ms after
incremental rendering (previous full-transcript runs measured about 19–29 ms and 80–156 ms respectively). Timings are
environment-dependent, not a performance guarantee; the runner enforces one render per burst, at most 24 DOM mutation
records for a six-line burst, retained row identity, and scroll anchoring rather than a machine-specific time threshold.
Browser checks also verify that selecting retained rows preserves their line separators for copying in all three engines;
rows use semantic block elements rather than relying on styled inline elements to produce native copied newlines.
These checks do not establish physical board qualification.

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
5. After a confirmed settings save or explicit reboot, verify the host reports `reboot_pending` and refuses further
   commands until reconnection. Repeat after Re-sync confirms an uncertain save/reboot; Re-sync must not resend it.
   Verify refused or failed saves do not leave the reboot gate set. Use only disposable settings and card data.
6. Shut down the host while idle, during a wireless test, and while a TCP console connection is authenticating.
   Confirm the opening socket closes, no AUTH response or session is published after shutdown, and the next launch
   reconnects normally. An already accepted HID operation may finish.
7. If duplicate-serial boards are available, rescan while both are connected. Verify the ambiguous identity disappears
   from the selector with a warning and previously obtained references cannot send commands; an unrelated board stays
   usable. Disconnect both and rescan within the 30-second serial-board grace period: absence must not remove the warning
   or enable commands through retained references. Reconnect only one and verify positive unique discovery clears
   quarantine and reuses the original monitor without replaying an unresolved command.
8. Record worst observed UI/report update delay, monitor process CPU, reconnect result, and any errors. Treat a failed
   step as a qualification gap; do not infer reliability from host unit tests.

Keep dated, configuration-specific observations in the [validation records](../validation/records/README.md), and
summarize outstanding coverage in [qualification gaps](../validation/qualification-gaps.md).

## Qualify the .NET Host with Hardware

The .NET monitor samples HID input every 100 ms; firmware telemetry, storage and self-test reports are emitted every
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
   and the UI recovers after completion or timeout. Cancel both during joining and as the test completes; verify
   cancellation is not automatically resent. If cancellation fails, confirm unrelated writes and console creation
   stay blocked across reconnection until explicit cleanup succeeds. Unknown delivery requires read-only Re-sync
   before cleanup can be retried. Do not use production credentials.
5. Measure first `/api/status` response latency and time from the first request to connected status with current
   telemetry. Record worst observed report/UI update delay, host CPU during idle and multi-board operation, command
   response latency, detach/reconnect results, and errors. To compare polling intervals, use otherwise equivalent host
   builds with 10 ms, 50 ms, and the current 100 ms interval; keep firmware, board, workload,
   observation duration, and host conditions fixed. Do not infer CPU reduction from wakeup counts alone. Treat failed
   steps as qualification gaps; host tests alone do not establish physical behavior.

Keep dated, configuration-specific observations in the [validation records](../validation/records/README.md), and
summarize outstanding coverage in [qualification gaps](../validation/qualification-gaps.md).

## Run Host Tests

Install the Python package first, then run the three hardware-free host suites together:

```sh
python3 -m pip install --editable host/python -r host/python/tests/requirements.txt
tools/validation/test-host.sh
```

The wrapper checks the Python dependency lock and focused strict types, then runs Python `unittest`, separate Direct
and Python/.NET frontend Node tests, and .NET tests.
They can also be run individually:

```sh
python3 -m unittest discover -s host/python/tests -v
python3 -m uv lock --check --project host/python
python3 -m pyright --project host/python/pyproject.toml
node host/direct/tests/run-tests.mjs
node host/web/tests/run-tests.mjs
dotnet test host/dotnet/PicoForge.sln --configuration Release --nologo --filter "Category!=Performance"
```

Python tests cover the package and local web API; .NET tests cover the protocol and host behavior; Direct tests cover
browser-side decoding and presentation. None opens a physical USB device. Repository-wide Python contracts are separate:

The shared dashboard also has browser-level checks for modal focus and Escape handling, narrow layouts, and Wi-Fi
temporary-test cancellation failure/retry. Serve `host/web` with a static HTTP server and open the check page in a
desktop browser:

```sh
python3 -m http.server 8000 --directory host/web
# Open http://127.0.0.1:8000/tests/browser/index.html
```

These checks use a mocked board API and do not qualify physical USB, firmware, or radio behavior. Keep device
qualification separate.

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
```

See the [test ownership index](../../tests/README.md) for cross-project checks and their prerequisites.

### Python host design

Report codecs are pure functions grouped by report family. Decoded shapes belong to `picoforge.reports.models`;
`picoforge.hid.models` preserves previous imports as compatibility exports. Annotation-only imports do not initialize
HID transport.
The HID client owns one handle, serializes native reads, Feature 20 transactions, disposal and wireless lifecycles,
and validates exact result
references without replaying uncertain writes. The monitor owns reconnects and snapshots, preserving unresolved command
references across client replacement. Routes validate HTTP requests and serialize public responses without modifying
internal snapshots. CLI module execution and the installed console script both delegate to the same entry point.
Public HID reads and close share the existing reentrant transaction lock. Close waits for accepted I/O, closes the
native handle once, and makes queued operations fail as closed; concurrent closes cannot retry failed cleanup.
Nested reads and forced-save reopening reuse that lock. Native calls still must return before close can finish.
The worker installs the active client and initial connected snapshot under the same command ownership; it removes the
client before publishing a disconnected snapshot under that ownership. Requests cannot use a partially installed
connection or have an accepted transition overwritten by startup publication.
LED publication also finishes before releasing command ownership, so an earlier request cannot overwrite a later
successful LED command in the snapshot.
One result-publication path applies `reboot_pending` for completed settings saves and explicit reboots, including when
subsequent saved-value or credential-commit validation fails. Those mismatches still return an error and do not publish
the requested settings as confirmed. Re-sync uses the same path without replaying the command. Refused, failed and
uncertain results do not claim completion. One shared gate refuses every subsequent command until reconnection resets
the snapshot; read-only status and Re-sync remain available.

Temporary Wi-Fi cleanup has a separate `wireless_cleanup_required` snapshot flag. A known cancellation failure does
not fabricate transaction uncertainty with missing reconciliation references. It retains cleanup ownership and blocks
unrelated commands until an explicit cancellation succeeds. Re-sync remains read-only: a completed cancel clears
cleanup, while a failed cancel leaves cleanup required and permits an explicit retry. An unknown cancel retains its
own exact transaction reference, not the cancelled test's terminal generation. Neither path replays the original test.
Discovery retirement retains monitors with cleanup pending, including across handle reconnection.

Successful worker or Re-sync wireless reads record a monotonic receipt time. Snapshots mark wireless status stale and
set `wireless_error` at six seconds without a successful refresh, without modifying stored report data. The six-second
bound leaves scheduling margin above the five-second idle poll; failed reads do not extend it. Console creation
rejects disconnected, failed, expired, or cleanup-required wireless state before opening a TCP connection.

Discovery runs outside the board registry lock; scans are serialized and publish only complete results, never after
shutdown. A duplicate identity is omitted from the selector and reported through optional discovery `warnings`; other
boards remain selectable. Its retained monitor rejects new commands, including through previously obtained references,
but does not interrupt accepted commands or discard uncertain outcomes. A complete unique rescan clears quarantine and
reuses the monitor; absent identities, unrelated boards, and failed enumeration cannot clear it. Quarantine for an
absent known identity ends only on positive unique discovery or a successful retirement claim that prevents new commands.
Unowned duplicate identities are forgotten when absent, rather than accumulating quarantine entries indefinitely.
Absent serial-numbered boards expire after 30 seconds, path-only boards after 5 seconds. Uncertain commands
retain their monitors until confirmed re-sync or shutdown. At most 16 monitors may be active or retiring. Each expired
monitor must first claim idle command ownership without waiting: active commands postpone expiry, unresolved outcomes
prevent it, and a successful claim refuses subsequent commands. Each claimed monitor has one retirement thread;
shutdown joins that thread instead of issuing overlapping cleanup. Failed retirement
is logged, retains the board's capacity slot, refuses reconnection, and is reported at shutdown without automatic retry.
If native HID close fails, the monitor stops, retains the failed client, publishes a cleanup error, and reports failure
from `close()`; it cannot open a replacement handle. Forced-save recovery transfers ownership of each candidate to the
HID client before opening or validating it, so even a failed candidate close stays owned. A native close failure is
terminal: subsequent commands and cleanup report it without retrying close or opening another handle. Restart is
required after unresolved cleanup failures.

Console, registry, monitor and snapshot models are strictly type-checked; compile-only negative contracts verify HID
serialization, command references, cleanup state, monitor command callbacks and route composition preserve argument and
return types. The HID client exposes the existing completed-result check as `require_completed`; its private spelling
remains an alias for compatibility. Route signatures are explicit; HID and route bodies remain incrementally typed.
Runtime checks remain at peer boundaries
even when an annotation promises valid data. Console sessions own one cancellable idle-expiry timer, which is cancelled
on disconnect or shutdown. Reading a TCP challenge has one 3-second deadline, not a per-byte timeout; authentication
including USB proof and sending the response shares an 8-second budget. Native HID calls must return for their software
deadlines to be enforceable. TCP connection setup has a separate 3-second timeout. Genuine console polling errors close
the session and return 503; a concurrent close is a 409, and an ordinary idle poll is successful with no output.
Shutdown also owns and closes an opening socket, including while USB proof is in flight. Authentication checks terminal
shutdown before proof and AUTH transmission; a completed proof cannot publish a session after shutdown.

Expected configuration-read I/O, protocol and board-state failures preserve identity and input diagnostics with a
`settings_message`. Configuration-dependent commands are blocked; LED control and read-only Re-sync remain available.
Unexpected `ValueError` or `RuntimeError` faults are logged as internal monitor errors, not degraded connected success.

Python request bodies are capped at 4 KiB, or 2 KiB for the console, with a JSON 413 error before board operations.
Settings, self-test and Wi-Fi test requests reuse the pure report validators before looking up or opening a board,
so malformed input returns 400 independently of board availability. Re-sync uses the same public wireless serializer
as status and Save, without modifying internal results.
Keep pinned runtime dependencies and `uv.lock` synchronized with `python3 -m uv lock --project host/python`.
Pinned contributor validators live in `host/python/tests/requirements.txt`, outside the shipped package.
Tests cover independent imports and module launching in fresh processes, retirement/shutdown races, native cleanup
failure, authentication deadlines, timer ownership, non-UTF-8 HID paths, body limits, and dependency-lock consistency.
Cross-operation tests combine the real monitor, HID client and HTTP routes over a fake native transport to verify reboot
gates, reconciliation, reconnection, cleanup retry and identity quarantine. Receipt-boundary tests cover wireless expiry,
refresh recovery and console admission. Event-controlled LED and connection publication tests verify command ownership
without timing sleeps; compound-failure tests cover candidate validation followed by native cleanup failure and terminal
monitor shutdown. The documented benchmark is also run as a subprocess.
These are hardware-free checks; polling, snapshot copying, and wireless lifecycle boundaries should change only after
measurement and physical qualification.

Host CI and release packaging run the same installed-wheel check with an isolated interpreter:

```sh
python -m pip wheel --no-deps host/python --wheel-dir build/wheels
python -m venv build/wheel-check
build/wheel-check/bin/python -m pip install build/wheels/*.whl
build/wheel-check/bin/python -I tools/validation/check-python-wheel.py 0.0.0.dev0
```

Run from the repository root; replace the expected version with the stamped version for releases. The check rejects
source imports, compares the complete installed asset payload with the shared source, serves pages and assets without
hardware, and exercises independent imports and both CLI entry points. On Windows use the environment's `Scripts`
directory instead of `bin`.

### Python dashboard performance baseline

Measure the dashboard hot paths before changing polling or snapshot copying. `--board` opens an attached board
read-only; stop any running dashboard first because it holds the HID interface:

```sh
host/python/.venv/bin/python tools/validation/bench-python-dashboard.py [--board [--serial SERIAL]]
```

The hardware-free workload decodes representative identity, configuration, runtime, storage, self-test and wireless
reports. Receipt timestamps are refreshed so freshness filtering cannot silently remove measured sections. The snapshot
measurement includes that refresh; the Flask test-client measurement includes request/serialization work, not network
transport. The CPU sample requires a live, connected, error-free worker that has actually polled the mocked device.
Without browser status requests the worker starts in its 500 ms idle cadence; this does not measure active 50 ms polling.
Board mode closes the monitor before separately measuring wireless reads with one exclusive HID handle.

| Path | Rate | Baseline (2026-11-06, x86-64 Linux, hardware-free) | Revisit when |
|---|---|---|---|
| `snapshot()` deep copy plus receipt refresh | Each `GET /api/status` | ~40 us | One status request exceeds 1 ms |
| `GET /api/status` through Flask test client | 1/s per visible tab | ~0.5 ms | Many tabs or noticeable status latency |
| 500 ms idle input poll loop | 2 wakes/s per board | ~0.5 ms CPU/s with mocked HID | Idle CPU exceeds ~1% per board |

These are local observations, not regression ceilings or hardware qualification. Real active/idle timing, report
freshness, multi-board CPU and USB/storage handoff remain subject to the
[hardware procedure](#qualify-the-python-host-with-hardware).

Save, scan and connection-test commands hold the device lock across several USB round trips (see
[command ownership](#command-ownership-and-cancellation)); time them by hand on hardware before splitting that lock, and
split it only where the write-safety rules still hold.

### .NET dashboard performance baseline

`PerformanceTests` times the status hot paths with a mocked board, including wireless state, using production JSON
options and application composition. Its injected clock stays fixed so telemetry cannot expire during measurement;
payload-shape assertions bracket the phases, including real HTTP responses. It fails only past generous regression
ceilings.
It is in the `Performance` category, which the default host suite skips; run it on its own:

```sh
dotnet test host/dotnet/PicoForge.sln -c Release --filter Category=Performance --logger "console;verbosity=detailed"
```

| Path | Rate | Baseline (2026-11-06, x86-64 Linux, Release) | Revisit when |
|---|---|---|---|
| `DeviceMonitor.Snapshot` (freshness applied on read) | Each `GET /api/status` | 0.29 us | One read exceeds 50 us |
| Status JSON serialization | Each `GET /api/status` | 19.0 us | One status exceeds 0.5 ms |
| `GET /api/status` through Kestrel | 1/s per visible tab | 0.26 ms | Many tabs or noticeable status latency |

HID commands block one thread-pool thread under the device lock, up to 35 s for a connection test. That is acceptable
for a localhost dashboard; move HID I/O to a dedicated thread only if concurrent requests start queueing.

### .NET host lifecycle

The board registry explicitly owns each dynamically started monitor's asynchronous disposal. Retirement must claim
idle command ownership without waiting, both for expired boards and ambiguous identities. Active commands postpone
retirement; unresolved results pin the monitor. Duplicate identities are quarantined immediately, independently of
retirement: registry lookup and new writes through retained references are refused while an accepted command may finish.
Only a successful scan finding a unique identity clears quarantine; absence or failed discovery does not. Retained
references can still perform read-only Re-sync without clearing the quarantine gate.
A successful retirement claim rejects subsequent commands, including calls through
previously obtained references. Retiring entries keep their identity and capacity slot until cleanup completes, and
reconnection cannot open another handle meanwhile. At most 16 active, retiring or cleanup-failed entries are retained.
Shutdown joins all cleanup tasks, including earlier retirements; one failure does not skip the remaining boards.

Native cleanup failure is logged and published as a disconnected cleanup error, stops the worker, and is reported
again at disposal without retrying close. The failed owner is retained; restart the dashboard before reconnecting.
Initial opens and forced-save reconnect candidates become owned before timeout configuration, report-size queries,
or identity validation. Failed initialization closes the opened stream; a failed initial close retains both causes
and the failed owner and stops the monitor without opening again. Rejected reconnect candidates
are closed before another is opened; a failed close is terminal, keeps the candidate owned, and stops reconnect retries.
Native HID calls must return before shutdown can finish; cancelling a request does not interrupt an accepted command.

After valid identity, an expected Feature 2 read failure preserves connected identity and input-report diagnostics.
`settings_supported` is false and `settings_message` explains the failure; wireless polling is disabled and
`wireless_error` carries the same diagnostic on wireless boards. Configuration-dependent commands return 409 without
writing. LED control and read-only Re-sync remain available. Configuration is read again on HID reconnection, not
retried every poll; unexpected parser bugs still produce a disconnected internal error and the normal retry cycle.

Confirmed settings/wireless saves and reboot commands set `reboot_pending`. All subsequent writes, including LED
and console authentication, are rejected until reconnection; telemetry and read-only Re-sync remain available.
Normal execution and Re-sync use one final-result publication path. It applies confirmed reboot effects even if a
later confirmation check fails; failed or uncertain commands do not invent completion. Result metadata retained for
publication excludes payload bytes, including TCP authentication proofs.

Temporary Wi-Fi cleanup has a separate `wireless_cleanup_required` snapshot flag. Known cancellation failure does not
invent delivery uncertainty: unrelated writes and console creation stay blocked, but explicit cleanup can be retried.
Unknown cancellation delivery retains the exact cancel transaction, not the cancelled test generation, and requires
read-only Re-sync first. A completed cancel clears cleanup ownership; a failed cancel leaves it required. Neither path
automatically resends cancellation. Cleanup ownership survives disconnect/reconnect and prevents monitor retirement.

The executable and integration tests use the same `DashboardApplication` composition. Tests exercise actual headers,
binding, body limits, error mapping, templates and static routes. Release smoke tests run the extracted executable,
compare the entire uncompressed dashboard payload against shared source, fetch assets, and verify hardware-free API
responses. These checks do not replace the [physical qualification procedure](#qualify-the-net-host-with-hardware).
The status OpenAPI contract includes HTTP 409 with the standard error body for registry conflicts such as quarantine,
retirement or exhausted monitor capacity.

Monitor polling and freshness limits are named constants, not runtime tuning options: input polling is 100 ms,
wireless polling is 500 ms while active and 2 s otherwise. Telemetry expires at 750 ms, storage and self-test at
1500 ms, and wireless at 5 s; polling intervals remain below their associated expiry limits. Wireless lifecycle tests
that attach snapshots use the manual clock, including cleanup HTTP tests, so startup cannot expire their fixtures.
Boundary tests check each report just before and exactly at expiry.

Wireless snapshots retain the last successful report for diagnostics, but a failed poll or a report at least five
seconds old sets `wireless_error`. Console creation requires a connected board with an error-free wireless snapshot
and no pending wireless cleanup;
an already-open page or direct API caller cannot bypass that policy. A successful poll clears the error and refreshes
the receipt time.

Console shutdown is terminal: it cancels opening authentication, closes both pending and active sockets, and prevents
a queued challenge continuation from starting USB proof or publishing a session. Proof already admitted before shutdown
may finish, but its result is not sent. Only one active or opening session is allowed. Connection setup has a 3-second
timeout; authentication has an 8-second total budget including the 3-second challenge limit, USB proof and response sending.
A synchronous native proof must return before its budget can be checked; late proofs are not sent.

Host regressions exercise duplicate discovery during accepted commands, retained-reference write gates, failed/stale
wireless console requests through the production HTTP composition, and challenge continuations queued across shutdown.
Native reconnect tests use controlled HidSharp devices and streams, including initial setup failures, compound
validation/close failures and terminal cleanup propagation through the real monitor. Wireless lifecycle regressions
run the real HID client, monitor and HTTP composition over fake native transport: active/late cancellation, final
failure versus unknown delivery, exact read-only Re-sync, explicit retries, reconnect retention, every write gate and
console refusal before dialing. They do not replace physical board qualification.

## Shared Frontend and Protocol Boundaries

Python and .NET serve the same assets from `host/web/src`; keep shared UI behavior backend-neutral and preserve the common
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
`host/web/src/js/dashboard.mjs` lifts them out for the view. Feature `12` wireless status arrives in the same snapshot as
`wireless` and `wireless_error`; the server polls it every 0.5 s during a scan or connection test and every 2 s
otherwise.

Every `/api` error body includes `{"error": "<message>"}`, and both servers map failures the same way:

| Status | Meaning | Python | .NET |
|---|---|---|---|
| 400 | Rejected input, before any HID write | `InvalidRequest` | `ApiInputException`, `JsonException`, bad binding |
| 404 | Unknown `board` ID | `LookupError` | `BoardNotFoundException` |
| 409 | Board state conflict or firmware refusal; retry after the state changes | `BoardConflict`, `WirelessTestCancelled` | `BoardStateException`, `BoardBusyException`, `WirelessTestCancelledException` |
| 413 | Body exceeds the request limit | Request size check | Kestrel or endpoint request size check |
| 503 | HID or TCP I/O, timeout, disconnect or malformed board data | `OSError`, `ProtocolError` | `IOException`, `TimeoutException`, `InvalidDataException`, `UnauthorizedAccessException`, `SocketException`, `OperationCanceledException` |
| 500 | Server fault; logged, and the message is not exposed | anything else, including ordinary `RuntimeError` | anything else |

Wi-Fi endpoints replace 503 transport detail with a fixed per-endpoint message, for example "Wireless provisioning
outcome is uncertain; command was not resent". Console connect reports "TCP console connection failed: <detail>" and
logs it as a warning. The board monitor reports faults in the
snapshot, prefixed "Internal monitor error" for a server fault, and reconnects after 1 s unless HID cleanup fails
(see [Python host design](#python-host-design) and [.NET host lifecycle](#net-host-lifecycle)). A stored `busy` control result
is history and does not block later commands; the per-device lock serializes them.

Each rule has one owner. Endpoints reject a malformed request with 400 whatever the board state; the HID client
guards firmware capabilities and report field lengths, and in Python also protocol ranges because the CLI calls it
directly (.NET has no CLI, so its endpoints are the only range check); the monitor checks only board state
(connection, storage ownership, self-test, pending reboot).
Both hosts cap request bodies at 4 KiB, with a tighter 2 KiB console limit; oversized API requests return JSON 413
before board lookup. Re-sync confirmation is validated before selecting the monitor.
Settings and Wi-Fi test bodies reject unknown fields. Wi-Fi test passwords may be omitted or empty for an open
network, but explicit null is invalid. The same malformed-request fixture is executed against both backends.

### Command ownership and cancellation

One command owns a board at a time. A scan holds it for up to 15 s and a connection test for up to 35 s, so an API
command waits at most 2 s for the board and then returns 409 "Board is busy with another command; retry when it
finishes"; .NET also stops waiting when the request is aborted. The monitor's own polling waits without a limit.

`POST /api/wireless/cancel` during a running test does not wait for the board. It flags the test, which sends opcode
`10` at its next 0.5 s poll, clears its in-flight uncertainty (the cancelled test never reports a result), and fails
with HTTP 409 "Wi-Fi test cancelled"; the cancel request returns `{"cancelled": true}` at once. A cancel that arrives after the
test's last poll is sent when the test returns. With no test running, cancel sends opcode `10` directly. The browser
waits 20 s for a scan (2 s board wait plus the 15 s scan) and 40 s for a test.

Command replies share a top-level key: `msc` for `POST /api/msc` and `/api/msc/force-return`, `settings` for
`POST /api/settings`, `networks`, `test` and `cancelled` for the Wi-Fi scan, test and cancel routes. Python also returns
the firmware `control_result`, which .NET omits. LED, reboot and self-test (202) replies are acknowledgements; the
browser applies the requested state on success and reads the outcome from the next status snapshot.

Non-GET `/api` requests with a foreign `Origin` return 403. The web console long-polls
`GET /api/console`; each asynchronous request waits up to 2 s for board output, honors request cancellation, and does
not block input.

### Direct dashboard independence

Direct is an independent product. It **must not share any code** with the Python/.NET dashboard: not HTML, CSS,
JavaScript, Python, renderers, controls, protocol codecs, transport adapters, the terminal bridge, assets, tests, or
test helpers. Do not introduce cross-dashboard imports, symlinks, shared component directories, build-time copying,
or copy-paste forks kept in sync with `host/web`, `host/python`, or `host/dotnet`. Similar behavior must be designed,
implemented, and maintained independently in Direct; behavioral changes do not require matching changes to the other.
The sole asset exception is the branding favicon: keep the two separately hosted `favicon.svg` files byte-identical,
without introducing build-time copying or a runtime dependency.

Direct code, including its Python-standard-library bridge, lives in `host/direct`. Python and .NET may continue sharing
their own frontend in `host/web`; this does not include
Direct. Common protocol behavior follows the firmware [HID Report Reference](../reference/usb-hid.md) and BLE profile, which
are specifications, not shared implementations. Repository CI may run both independent test suites.
The [dashboard isolation contracts](../../tests/contracts/test_dashboard_isolation.py) check module/asset boundaries,
that the Direct bridge imports nothing from the native hosts, that no Direct file except the branding favicon is a
byte-identical copy of a Python/.NET/web host file, and that the native hosts do not package or serve Direct.
Near-copies are a review concern.

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
