# PicoForge Direct Dashboard

Connect to a PicoForge board directly from desktop Chrome or Edge using **USB (WebHID)** or
**Bluetooth LE (Web Bluetooth)**. View live diagnostics, edit boot settings, and run supported board controls
without installing the Python or .NET host application.

The optional Wi-Fi CLI console requires the included local server. The dashboard itself uses plain browser
ES modules: no bundle, build step, or runtime JavaScript package installation is required.

- [Quick start](#quick-start)
- [Dashboard layout](#dashboard-layout)
- [Controls](#controls)
- [Page lifecycle](#page-lifecycle)
- [Protocol and connection](#protocol-and-connection)
- [Architecture](#architecture)
- [Checks](#checks)
- [Further reading](#further-reading)

## Quick start

### Choose how to run it

| Option | Start here | USB / Bluetooth | Wi-Fi CLI console |
|---|---|---|---|
| Hosted dashboard | [Open the dashboard](https://djwinter29-oss.github.io/PicoForge/) | Yes, with browser support | No |
| Repository checkout | Run the command below from the repository root | Yes, with browser support | Yes |
| Release ZIP | Extract `picoforge-direct-vX.Y.Z.zip` from [Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest) | Yes, with browser support | Yes, with the included server |

**From a repository checkout:**

```sh
tools/host/run-direct.sh --port 8000
```

**From an extracted release ZIP:**

```sh
python3 server.py --port 8000
```

For either local option, open **<http://127.0.0.1:8000/>**. The local server requires Python, but does not require the
PicoForge Python host package. Keep the default loopback binding for local use.

Opening `index.html` as a `file://` URL is not supported. WebHID and Web Bluetooth require a supported browser and
OS in a secure context, such as HTTPS or localhost. The hosted site is static; it does not run the terminal bridge.

### Connect a board

1. Close other applications using the board's HID interface.
2. Choose **USB**, then **Select board**.
3. Select the PicoForge board and grant access to its vendor HID interface.
4. Wait for valid identity, configuration, and fresh status reports before using controls.

The chooser opens even for previously authorized devices so you can select the intended board. Cancel leaves the
dashboard disconnected. Browser permission cannot override another application's HID access.

For BLE-capable boards, first provision a Control key over USB. Then choose **Bluetooth**, enter the saved key,
and scan for the board. See [wireless setup](../../docs/guides/wireless.md).

### Safety essentials

- Do not run competing clients against the same board.
- A timeout or disconnect does **not** automatically retry a command. Resolve uncertain outcomes before continuing.
- Back up card data. Stop host file activity and safely eject whenever possible before returning USB storage.
- Control keys authenticate commands; they do not encrypt BLE or TCP traffic. Flash credentials are not encrypted.
- Host tests and successful builds do not establish physical hardware reliability.

## Dashboard layout

The layout follows the other PicoForge dashboards, but its implementation is independent.

| Section | What it shows or controls |
|---|---|
| Device information | Product, board/MCU, serial, system clock, reserved/total firmware RAM, and firmware version |
| Board configuration | Next-boot clock, SD-card profile, radio switches, TCP CLI switch, and reset cause |
| Pin configuration | Collapsible Monitor UART and Storage GPIO tables |
| Live diagnostics | Uptime, Core 0/Core 1 service load, temperature, CLI port, and wireless status |
| Storage | Actual bus clock/timing, card state, and USB ownership |
| Storage benchmark | Optional write/verify progress and results |

Firmware RAM is boot-stable build accounting, not live heap or stack usage. The stored SD-card profile is a
request; Storage shows the effective timing actually selected.

During busy sampling, the view may retain the last valid card details. Stale reports, disconnects, ownership
transitions, and valid absent/uninitialized samples clear them. Retained values are display-only and never
authorize a command.

### Wi-Fi CLI console

When Wi-Fi and TCP CLI are connected, click the Wi-Fi value to open the console in a new tab.
Use **Enter** to run commands, **Tab** to complete them, and arrow keys to edit or recall history.

Run the included local server, not a generic static-file server. The bridge supports one terminal session and
closes it after 15 seconds without a request. Traffic to the board uses unencrypted TCP port 2323.

Keep the originating dashboard connected. Each console connection or reconnection requests a fresh, one-use
authentication proof from that same-origin dashboard; the URL contains the board address, not a Control key.
Reloading the dashboard requires reopening its consoles. See the
[console guide](../../docs/guides/console.md#direct-dashboard-authentication) for authentication and security limits.

## Controls

Controls depend on board capabilities, connection type, fresh reports, and storage readiness.

| Action | USB | Authenticated BLE |
|---|---|---|
| Clock, SD-card profile, and TCP CLI settings | Supported | Supported |
| Confirmed reboot | Supported | Supported |
| Radio switches, Wi-Fi credentials, and Control-key provisioning | Supported on wireless boards | USB-only |
| Forced save while USB storage is shared | Supported with explicit confirmation | USB-only |
| USB storage handoff and benchmark execution | Supported when eligible | USB-only |
| Benchmark status | Readable | Readable |

### Wireless provisioning

Pico W and Pico 2 W support optional Wi-Fi/BLE provisioning:

1. Over USB, choose **Configure…** for the radio.
2. For Wi-Fi, select or enter a network and successfully test it.
3. For Bluetooth, generate a Control key and save a copy before committing it.
4. Use **Save&Reboot** to persist the changes.

Save a successfully tested Wi-Fi pair before its staging lifetime expires, normally within ten minutes.
Saving the tested pair does not restage it. A failed replacement test invalidates the previous tested pair and
blocks Save until another test succeeds or cancellation clears the temporary credentials.

If cancellation fails, the dialog stays open and Save remains blocked until a retry or verified reconnect result
confirms clearing. A failed replacement-key save wipes submitted key text; regenerate it or disable Bluetooth to
abandon the replacement. Disabling a configured radio erases its saved credentials on a successful save.

The form displays its Save-block reason and uses the same reason for eligibility, request rejection, and the Save
button title. Full provisioning behavior is documented in the [wireless guide](../../docs/guides/wireless.md).

### Saving and rebooting

**Save&Reboot** requires confirmation and verifies the settings command's completion before reporting success.
Firmware persists the settings and schedules a reboot; configuration is reread after reconnection.
**Reboot Board** is a separate, capability-gated action.

Cancel or Escape sends no command. Safety gates are checked again at confirmation, not just when opening a dialog.
Local settings editing remains available while USB storage is shared, but forced saving requires an additional
host-cache data-loss confirmation. Sync/detach must succeed before flash programming. Credential changes cannot
use that forced-save path. See the [forced-save policy](../../docs/reference/settings.md#forced-save-and-reboot).
If wireless changes block Save while storage is shared, the button title and visible settings note explain the
required sequence: stop host file activity, safely eject, then confirm **Return to Pico** before saving.

### Storage and benchmarks

- **Enable USB drive** confirms exclusive host access before handing over storage.
- **Return to Pico** is an explicitly confirmed force return, **not** an OS eject.
- USB ownership transitions keep CDC, HID, and MSC interfaces connected. Pico-owned storage reports no medium to MSC.
- Healthy firmware-owned storage mounts at startup and automatically remounts after normal safe USB return.
- The benchmark requires fresh status and an already-mounted filesystem. Start authorizes temporary `/PFTEST.BIN`
  writes, not mounting; an existing file is rejected rather than replaced.
- Free-space scanning may delay benchmark acceptance. Wait for its result instead of clicking Start again.
- Shared storage and active/pending benchmarks restrict conflicting actions.

The dashboard does not automatically format or repair a card. Reconnection alone does not prove a successful
remount. See the [storage lifecycle](../../docs/reference/architecture.md#storage-ownership-lifecycle) and
[force-return risks](../../docs/reference/usb-hid.md#msc-force-return-safety).

## Page lifecycle

Navigating away releases board resources and closes the console's TCP session. Returning through the browser's
history cache reloads into a disconnected state: drafts and console scrollback are cleared; reconnect explicitly.
Commands and terminal input are never automatically replayed.

Pending-operation journals remain in the current tab's `sessionStorage` for same-board reconciliation.
They contain allowlisted transaction/profile/commit-mask or generation facts, never SSIDs, passwords, or Control
keys. Missing, inaccessible, or invalid journal storage blocks writes. Keep the tab open while resolving an
uncertain operation; recovery does not survive ending the tab's session.

Async form results belong to their originating session, connection generation, and draft lifecycle. Old
completions cannot clear newer post-reconnect drafts. Clipboard feedback also checks the key's local revision,
without retaining another secret copy. A forced save may report verified same-session recovery after reconnect
only when no newer settings or wireless draft exists.

The Wi-Fi expiry countdown updates locally once per second and notifies Save readiness without waiting for another
board report. Expired credentials must be tested again.

## Protocol and connection

The dashboard uses version 1 of the [HID Report Reference](../../docs/reference/usb-hid.md).
Mandatory identity and configuration reports must validate before commands are enabled.

- WebHID provides payload bytes without the report ID. Exact report lengths are required; framing is not guessed.
- Control operations serialize Feature 20 SET/GET transactions and match both transaction ID and opcode.
- Acceptance alone does not prove a storage handoff or benchmark completion; fresh corresponding Input status is
  required. Save results must confirm the requested switches and wireless commit mask.
- Reconnect identifies the board through Feature 1 and its stable UID, then reads outstanding results without SET
  replay. Accepted scan/test lifecycles reconcile their generation through Feature 12.
- BLE maps the shared envelope to its restricted nine-byte command/result form.
- Telemetry expires after 750 ms; storage and self-test reports expire after 1500 ms.
- Wireless status polls every 500 ms during scan/test and every two seconds while idle; it expires after three
  polling intervals.
- Scanned SSID bytes round-trip through `ssid_hex`; escaped `ssid` text is a display label.

Unavailable and stale reports are identified separately. Unknown outcomes, missing generations, or mismatched
recovery results keep writes blocked.

## Architecture

Direct owns its HTML, CSS, favicon, browser controllers, and terminal bridge. It uses neither Web Serial nor
Python/.NET dashboard modules. See the authoritative
[Direct independence policy](../../docs/development/host-tools.md#direct-dashboard-independence).

| Source | Responsibility |
|---|---|
| `server.py` | Local asset serving and optional terminal bridge |
| `web/js/app.mjs`, `console-app.mjs` | Browser composition roots |
| `web/js/protocol/` | Report codecs, opcode policy, and console-authentication messages |
| `web/js/transport/` | Abort-aware HID/BLE byte pipes, framing, and link authentication |
| `web/js/board/` | Session facade, connection lifecycle, command journal, recovery, polling, and use cases |
| `web/js/state/` | Domain/UI stores, allowlisted events, readiness selectors, and errors |
| `web/js/ui/` | Dashboard/console composition, view models, and DOM updates |
| `web/js/timers.mjs` | Abortable waits and page-owned timer scopes |

### Ownership rules

- Protocol and state do not access the DOM or browser device APIs. Composition roots supply browser dependencies.
- Board modules write domain state through declared events; only the connection module advances connection phases.
- Commands are journalled before writing. The single-flight queue rejects overlaps rather than buffering stale
  intents. Link policy belongs to opcode descriptors, not transports.
- UI gating and use cases share `canRun`. Rendering reuses one eligibility snapshot; device actions check live state.
- Domain errors/progress use reason codes; `ui/messages.mjs` converts them to display text. Browser errors retain
  their raw detail under `UNEXPECTED`.
- Dashboard-only state uses a separate UI store. Wireless drafts/phases stay local to the form controller.
- Rendering performs no device I/O. Storage-retention caches belong to the mounted view, not global state.
- Required DOM lookups use a typed per-document cache. Tag-qualified selectors provide native element types;
  missing, non-HTML, or wrong-tag results fail explicitly.

Wireless provisioning keeps three focused modules: `wireless-form.mjs` owns Wi-Fi operations, lifecycle, and combined
save composition; `ble-key-form.mjs` owns key generation, clipboard feedback, and secret disposal;
`wireless-form-view.mjs` renders a snapshot. Keep tightly coupled Wi-Fi staging, cancellation, expiry, and recovery
together rather than splitting them solely to shorten files.

The terminal retains at most 1000 rows of 512 columns, caches unedited row text, and reuses three text-only DOM nodes.
Retired row caches are garbage-collectable. It is a bounded firmware CLI renderer, not a full terminal emulator.
Live console output shares one pending animation frame; input transmission does not wait for that frame.
Disconnect flushes final output and cancels its pending frame; navigation cancels it without rendering stale data.
Bridge responses are validated per operation before adoption, including exact write acknowledgments. Malformed
responses and partial writes surface errors rather than being retried.

## Checks

Run commands below from the **repository root**. These are development checks, not a firmware build or hardware
qualification.

### Host tests and type checks

```sh
node host/direct/tests/run-tests.mjs
python3 -m unittest discover -s host/direct/tests -p 'test_*.py'
npx --yes --package typescript@5.9.3 tsc --project host/direct/web/js/tsconfig.json
```

JavaScript tests mirror `web/js` paths; for example, `board/commands.mjs` maps to
`tests/board/commands.test.mjs`. To isolate a failure:

```sh
node --test host/direct/tests/board/commands.test.mjs
node host/direct/tests/run-tests.mjs --test-name-pattern="timeout"
```

The runner recursively discovers `*.test.mjs`. Architecture/layout contracts check source matching and ownership.
Form tests cover failed staging blocking Save, expiry, reset invalidating late continuations, and known cancellation
clearing staging without replay. Shared DOM fixtures use additive listeners and await asynchronous handlers.

| Test directory | Scope |
|---|---|
| `tests/integration/` | Composition roots and cross-layer scenarios |
| `tests/contracts/` | Architecture and test-layout invariants |
| `tests/web/` | HTML and asset contracts |
| `tests/browser/` | Chromium interaction checks and their pinned dependency |
| `tests/types/` | Static-only negative type contracts; never run with Node |
| `tests/support/` | Shared Direct-only fixtures |
| `tests/test_server.py` | Root-level server behavior |

The no-emit JSDoc check covers protocol/state, transports, timers, dashboard/form composition, and console rendering.
Negative contracts reject invalid state, configuration, callback, environment, DOM, selector, and console-response
types. Typing is incremental:
the wider configuration still permits implicit `any`; not every UI module is fully typed.

### Browser checks

Install the pinned test-only dependency and Chromium, then run:

```sh
python3 -m pip install -r host/direct/tests/browser/requirements.txt
python3 -m playwright install chromium
python3 host/direct/tests/browser/run.py
```

The runner owns an ephemeral loopback server and fails on incomplete checks, assertion failures, or browser errors.
No browser-automation dependency is required by the dashboard at runtime.

Coverage includes dialog focus/Escape, narrow viewports, invalid keys, Wi-Fi selection/cancellation/reset,
replacement tests, failed-key-save recovery, shared-storage Save blockers, and save/reconnect ownership.
Fresh iframe fixtures prevent duplicate
controller binding. Actual dashboard/console history navigation checks verify draft/output disposal, journal
retention, and no terminal requests. WebHID is disabled for those history checks because its listeners prevent
Chromium history-cache eligibility; they do not qualify connected hardware.

Performance checks assert one uptime update per three-event telemetry burst, no unchanged connection-label or
provisioning-feedback text writes, stable terminal nodes, bounded scrollback, and correct cursor output after
streaming. Median/p95 timings are diagnostics, not portable pass thresholds. Streaming measurements separate parsing,
row-text construction, the `scrollHeight` layout read, and remaining DOM/scroll work. The remainder includes overhead
and any other layout; independent phase percentiles must not be summed.
Terminal measurements use production HTML/CSS in a 1000-by-800 iframe with connected presentation, focus, overflow,
and bottom-scroll assertions, but no live TCP session. Integration tests separately verify frame coalescing, immediate
input, final-output flushing, navigation cleanup, and no replay after a partial write. Coalescing reduces redundant
renders when output accumulates before a frame; it does not claim to reduce the cost of an individual layout.

To inspect the browser checks manually:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory host/direct
```

Open <http://127.0.0.1:8000/tests/browser/index.html>. This test server serves assets only; it does not provide the
terminal bridge.

### Hardware qualification

Host and browser mocks do not qualify physical USB/BLE timing, staging cleanup, or forced-save behavior.
Follow the [Direct board procedure](../../docs/development/firmware-testing.md#direct-dashboard-board-checks).
Record each board/OS/browser combination separately, and mark untested combinations as qualification gaps.

## Further reading

- [Wireless setup](../../docs/guides/wireless.md)
- [Console authentication and security limits](../../docs/guides/console.md#direct-dashboard-authentication)
- [HID report reference](../../docs/reference/usb-hid.md)
- [Storage lifecycle](../../docs/reference/architecture.md#storage-ownership-lifecycle)
- [Forced-save policy](../../docs/reference/settings.md#forced-save-and-reboot)
- [Host development and Direct independence](../../docs/development/host-tools.md#direct-dashboard-independence)
- [Hardware qualification gaps](../../docs/validation/qualification-gaps.md)
- [Release and Pages publishing](../../docs/deployment/release.md#publishing-setup)
