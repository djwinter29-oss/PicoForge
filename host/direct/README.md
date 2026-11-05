# PicoForge Direct Dashboard

The Direct dashboard connects from the browser to the board over USB (WebHID) or Bluetooth LE
(Web Bluetooth). The optional Wi-Fi CLI terminal needs the Direct-only local bridge.

Open the hosted [Direct dashboard](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome or Edge.
The hosted site is deployed from successful releases after the initial
[Pages setup](../../docs/deployment/release.md#publishing-setup).
For offline/local hosting, download the Direct dashboard ZIP (`picoforge-direct-vX.Y.Z.zip`) from
[Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest), extract it, and serve the extracted directory
on localhost. Opening `index.html` directly as a file is not supported. The hosted dashboard supports USB and
Bluetooth diagnostics and controls, but cannot access the local Wi-Fi terminal bridge.

From a repository checkout, use the local runner, which serves the Direct files and optional Wi-Fi terminal bridge:

```sh
tools/host/run-direct.sh --port 8000
```

The downloadable Direct ZIP also includes `server.py`; run `python3 server.py --port 8000` from the extracted
directory to enable the same local terminal. The hosted GitHub Pages dashboard is static and does not deploy this
server. It supports browser USB and Bluetooth diagnostics and controls, but not the Wi-Fi terminal.

Open `http://127.0.0.1:8000/` in a WebHID-capable browser and grant access to the vendor HID interface.
The page owns its HTML, CSS, view logic, and favicon; it does not use Web Serial or any shared-dashboard module.
It must not share any dashboard components with Python/.NET; see the authoritative
[dashboard component isolation policy](../../docs/development/host-tools.md#dashboard-component-isolation).
Choose **USB**, then **Select board** to open the browser device chooser, including when devices were previously
authorized, so the user selects which board to open. Cancel leaves the page disconnected. Close other clients
using that board before connecting; the chooser cannot override another application's HID access.

## Protocol and connection

The firmware and all host dashboards use version 1 of the authoritative
[HID Report Reference](../../docs/reference/usb-hid.md). Physical USB qualification remains outstanding.
It requires Feature 1 identity (26 payload bytes), Feature 2 board configuration (17), Feature 20 control/result
(68), wireless Feature 12 (48, capability bit 7), and Input 3 telemetry (22), 10 storage (34), and optionally
11 self-test (32, capability bit 5).
Mandatory identity and configuration reports must be valid before the dashboard enables commands.

WebHID supplies payload bytes without the report ID. Exact lengths are required; this adapter does not guess framing
from byte values or discard padding. Telemetry expires at 750 ms, storage and self-test at 1500 ms.
Stale measurements are removed, unavailable/stale freshness is shown per report, and disconnect clears
connection caches. Wireless status is polled every 500 ms during tests/scans and every two seconds while idle;
it expires after three polling intervals. Raw SSID bytes round-trip through `ssid_hex`; the escaped `ssid` label
is display-only for scanned networks.
Firmware RAM reserved / total is boot-stable build accounting, not live heap or stack usage.

## Dashboard layout

The page follows the Python/.NET dashboard: a six-field Device information row (product, board/MCU, serial,
system clock, firmware RAM reserved / total, firmware version) and a Board configuration row with clock, SD-card
profile, and reset cause. Pin configuration starts collapsed and contains full-width, vertically stacked Monitor
UART and Storage tables. Each table has signal headings above one GPIO row.

Live diagnostics shows uptime, service load for Core 0 / Core 1, board temperature, then CLI port. Storage combines
effective bus clock and timing in one field. Busy sampling retains the last valid card details without inserting
a message or shifting the table; stale reports, disconnects, ownership transitions, and valid absent/uninitialized
samples clear retained details. Retained values are display-only and never authorize a command.
When Wi-Fi and the board TCP CLI are connected, click the Wi-Fi value to open the Wi-Fi console in a new tab. Like
the Python dashboard console, it is an interactive terminal: Enter runs a command, Tab completes, and arrow keys edit
and recall history. It relays raw keystrokes over unencrypted TCP port 2323. The console reuses the matching Direct
dashboard's board authentication instead of asking for the Control key again; keep that dashboard connected until
the console authenticates. The bridge permits one terminal session and closes it after 15 seconds without a
request. Run the included server instead of a generic static-file server to enable this feature. See the
[console guide](../../docs/guides/console.md#direct-dashboard-authentication) for the rationale and security limits.

## Controls

Pico W/Pico 2 W also support optional [Wi-Fi configuration and BLE connections](../../docs/guides/wireless.md).
Use USB to provision radio settings and a random Control key, then open **Bluetooth**, enter the key and scan;
Bluetooth stays off without a saved key. See the linked wireless guide for the key-gated session.
The authenticated Bluetooth session can change system clock, SD-card speed, and the TCP CLI enable switch, and can
request a confirmed reboot. Radio switches, Wi-Fi credentials, Control-key provisioning, forced saves, and USB
storage ownership and benchmark execution remain USB-only. Benchmark status remains readable over BLE.
BLE maps the unified USB envelope to its restricted nine-byte command/result form. Successful temporary Wi-Fi
tests must be saved within ten minutes; saving a tested pair does not restage it.

All operations serialize Feature 20 SET/GET transactions, matching both transaction ID and opcode.
An acceptance result does not prove storage ownership changed or a self-test finished; the dashboard waits for
fresh corresponding Input status. A timeout or disconnect never replays a command; uncertain results block further
writes. Reconnecting reads the outstanding result without resubmitting it. Do not run competing HID writers.
Wireless staging, tests, scans, saves and TCP CLI proofs share that same serialized Feature 20 channel.
Save results must confirm exact switches and the wireless commit mask, even after firmware clears staging.
Accepted scans/tests retain their generation and read Feature 12 after same-board reconnect. Matching terminal
results resolve uncertainty; missing or changed generations keep wireless operations blocked. These operations
are never resent automatically.
If clearing temporary Wi-Fi test credentials fails, the Wi-Fi dialog stays open and saving remains blocked until
the user retries or reconnect confirms cancellation.
Pending operations are also retained in the current tab's `sessionStorage` so a page reload can resume same-board
reconciliation. Recovery records contain only allowlisted transaction/profile/commit-mask or generation data, never the
SSID,
Wi-Fi password, or Control key. Missing, inaccessible, or invalid recovery storage fails closed and blocks writes
rather than allowing an untracked operation. Dialog state and secrets are not restored after a page reload.
Keep the tab open until an uncertain operation is resolved; recovery does not survive ending the browser tab's session.
USB sharing hides the benchmark from submission until fresh status confirms the requested
ownership and a finished transition, or an explicit rejection. Self-test acceptance keeps controls pending until
a newer status report, including sequence rollover. Active/pending tests disable settings controls without a
routine waiting notice; Save availability indicates edited settings without a redundant unsaved-changes notice.

**Save&Reboot** confirms with the user, sends one settings command, and verifies its completion result.
Firmware saves the settings and schedules the reboot; Feature 2 is reread after reconnection.
**Reboot Board** is separate, capability-gated, and explicitly confirmed. Save, reboot, and Return to Pico use
matching modal dialogs with Cancel and Escape support; no command is sent until confirmed, and safety gates are
checked again at confirmation. Routine loaded-settings and fresh-report messages are hidden.
Standard/High-Speed is a stored request;
Storage shows the actual selected timing and effective bus clock.
While shared, local settings editing remains available. Firmware permits **Save&Reboot** with
an additional explicit host-cache data-loss confirmation; sync/detach must succeed before flash programming.
The reconnect path verifies the original transaction and saved values without replaying SET. See the
[forced-save policy](../../docs/reference/settings.md#forced-save-and-reboot). Self-test and transition gates still
apply.

USB ownership transitions leave CDC, HID and MSC connected; Pico-owned storage reports no medium through MSC.
See the [storage lifecycle](../../docs/reference/architecture.md#storage-ownership-lifecycle).
After a reboot or forced save, the dashboard identifies a reconnect candidate by reading Feature 1 and matching its
stable device UID before resuming; it never replays the prior command.
**Return to Pico** is explicitly confirmed force return, not an OS eject.
**Enable USB drive** first opens a matching storage-handoff dialog explaining exclusive host access and safe eject;
Cancel or Escape sends no command, and confirmation rechecks the current safety gates.
Stop host file activity and safely eject whenever possible first. See the
[force-return risks](../../docs/reference/usb-hid.md#msc-force-return-safety).
The card is not automatically formatted or repaired, and reconnect does not prove successful remount.

Firmware mounts at startup, keeps healthy firmware-owned storage mounted, and automatically remounts after normal
safe USB return; see the [storage lifecycle](../../docs/reference/architecture.md#storage-ownership-lifecycle).
Sharing transfers exclusive access to the host; neither firmware files nor self-test are available then.
The optional self-test requires fresh, known self-test status and an already-mounted filesystem.
Start authorizes temporary `/PFTEST.BIN` writes, not mounting; an existing file is rejected, never replaced.
Preflight free-space scanning can delay acceptance before progress appears. Wait for the matching command result
instead of clicking Start again. Cleanup leaves the filesystem mounted.

## Checks

```sh
node --test host/direct/tests/*.test.mjs
python3 -m unittest discover -s host/direct/tests -p 'test_*.py'
```

For browser interaction checks from a repository checkout, serve the Direct dashboard and open
`http://127.0.0.1:8000/tests/browser.html` in a supported browser:

```sh
tools/host/run-direct.sh --port 8000
```

The page reports dialog focus and Escape behavior, narrow viewport overflow, and Wi-Fi cancellation failure/retry
using mocked device operations. These browser checks do not replace physical WebHID/USB qualification, which still
requires firmware implementing the version-1 reports and RP2040/RP2350 hardware.