# Direct Dashboard Development

[Documentation](../README.md) / [Development](README.md)

For installation, connection, and controls, use the [Direct user guide](../../host/direct/README.md).
Direct has its own code, bridge, assets, and tests; follow the
[independence policy](host-tools.md#direct-dashboard-independence).

Run commands below from the repository root.

On this page: [source layout](#source-layout) / [freshness](#polling-and-freshness) /
[unit and type checks](#host-tests-and-type-checks) / [browser checks](#browser-checks) /
[hardware checks](#hardware-qualification).

## Source Layout

Paths in this map are relative to `host/direct/`.

| Location | Responsibility |
|---|---|
| `server.py` | Local asset serving and optional terminal bridge |
| `web/js/app.mjs`, `console-app.mjs` | Browser composition roots |
| `web/js/protocol/` | Report codecs, opcode policy, and console-authentication messages |
| `web/js/transport/` | HID/BLE byte pipes, framing, and link authentication |
| `web/js/board/` | Session facade, connection lifecycle, command journal, recovery, polling, and use cases |
| `web/js/state/` | Domain/UI stores, events, readiness selectors, and errors |
| `web/js/ui/` | Dashboard/console composition, view models, and DOM updates |

Keep protocol and state independent of the DOM and browser device APIs. Commands are journalled before writing; the
single-flight queue rejects overlaps rather than buffering stale intents. Rendering performs no device I/O, and
malformed bridge acknowledgments remain errors rather than replay triggers. Wireless forms own staging and
cancellation; the view renders a snapshot. The terminal is a bounded firmware-CLI renderer, not a general-purpose
terminal emulator.

## Polling and Freshness

Wireless freshness is client policy, not part of the HID report contract:

| Wireless state | Poll interval | Snapshot expires after |
|---|---:|---:|
| Scan or test active | 500 ms | 1.5 s |
| Otherwise | 2 s | 6 s |

Expiry is three times the current poll interval. Input-report freshness follows the
[HID reference](../reference/usb-hid.md#input-freshness).

## Host Tests and Type Checks

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
Form tests cover staging, expiry, cancellation, and reset invalidating late continuations. Shared DOM fixtures use
additive listeners and await asynchronous handlers.

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
types. Typing is incremental: the wider configuration still permits implicit `any`; not every UI module is fully typed.

## Browser Checks

Install the pinned test-only dependency and Chromium, then run:

```sh
python3 -m pip install -r host/direct/tests/browser/requirements.txt
python3 -m playwright install chromium
python3 host/direct/tests/browser/run.py
```

The runner owns an ephemeral loopback server and fails on incomplete checks, assertion failures, or browser errors.
No browser-automation dependency is required by the dashboard at runtime.

Checks cover control admission, provisioning/recovery, and dashboard/console history navigation. Fresh iframe fixtures
prevent duplicate controller binding. History checks verify draft/output disposal, journal retention, and no terminal
requests. WebHID is disabled for those checks because its listeners prevent Chromium history-cache eligibility;
they do not qualify connected hardware.

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

## Hardware Qualification

Host and browser mocks do not qualify physical USB/BLE timing, staging cleanup, or forced-save behavior.
Follow the [Direct board procedure](firmware-testing.md#direct-dashboard-board-checks).
Record each board/OS/browser combination separately, and mark untested combinations as
[qualification gaps](../evidence/README.md).
