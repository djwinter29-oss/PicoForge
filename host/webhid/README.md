# PicoForge WebHID

Open the hosted [WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome or Edge.
The hosted site is deployed from successful releases after the initial
[Pages setup](../../docs/deployment/release.md#publishing-setup).
For offline/local hosting, download the WebHID ZIP from
[Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest), extract it, and serve the extracted directory
on localhost. Opening `index.html` directly as a file is not supported.

Serve the WebHID directory directly so its `index.html` is the site root and the browser gets a secure localhost origin:

```sh
python3 -m http.server 8000 --directory host/webhid
```

Open `http://localhost:8000/` in a WebHID-capable browser and grant access to the vendor HID interface.
The page owns its HTML, CSS, view logic, and favicon; it does not use Web Serial or a host server API.
Every explicit **Connect** opens the browser device chooser, including when devices were previously authorized,
so the user selects which board to open. Cancel leaves the page disconnected. Close other clients using that board
before connecting; the chooser cannot override another application's HID access.

## Protocol compatibility

This standalone client targets layout version 1 of the authoritative
[HID Report Reference](../../docs/design/usb/usb-hid-report-reference.md), implemented by firmware and the Python,
.NET, shared dashboard, and direct WebHID clients. Physical USB qualification remains outstanding.
It requires Feature 1 identity (26 payload bytes), Feature 2 board configuration (17), Feature 20 control/result
(9), and Input 3 telemetry (22), 10 storage (34), and optionally 11 self-test (32, capability bit 5).
Mandatory identity/configuration failures are explicit connection errors, not legacy fallbacks.

WebHID supplies payload bytes without the report ID. Exact lengths are required; this adapter does not guess framing
from byte values or discard padding. Telemetry expires at 750 ms, storage and self-test at 1500 ms.
Stale measurements are removed, unavailable/stale freshness is shown per report, and disconnect clears connection caches.
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

## Controls

Pico W/Pico 2 W also support optional [Wi-Fi configuration and BLE connections](../../docs/design/wireless.md).
Use USB to provision radio settings and a random BLE key, then select Bluetooth LE for public diagnostics and
key-authorized LED/benchmark controls. Privileged management remains USB-only. Radios default off.

All operations serialize Feature 20 SET/GET transactions, matching both transaction ID and opcode.
An acceptance result does not prove storage ownership changed or a self-test finished; the dashboard waits for
fresh corresponding Input status. A timeout or disconnect never replays a command; uncertain results block further
writes. Reconnecting reads the outstanding result without resubmitting it. Do not run competing HID writers.
USB sharing hides the benchmark from submission until fresh status confirms the requested
ownership and a finished transition, or an explicit rejection. Self-test acceptance keeps controls pending until
a newer status report, including sequence rollover. Active/pending tests disable settings controls without a
routine waiting notice; Save availability indicates edited settings without a redundant unsaved-changes notice.

**Save and Reboot** confirms with the user, sends one settings command, and verifies its completion result.
Firmware saves the settings and schedules the reboot; Feature 2 is reread after reconnection.
**Reboot Board** is separate, capability-gated, and explicitly confirmed. Save, reboot, and Return to Pico use
matching modal dialogs with Cancel and Escape support; no command is sent until confirmed, and safety gates are
checked again at confirmation. Routine loaded-settings and fresh-report messages are hidden.
Standard/High-Speed is a stored request;
Storage shows the actual selected timing and effective bus clock.
While shared, local settings editing remains available. Supporting firmware permits **Save and Reboot** with
an additional explicit host-cache data-loss confirmation; sync/detach must succeed before flash programming.
The reconnect path verifies the original transaction and saved values without replaying SET. See the
[forced-save policy](../../docs/design/settings.md#forced-save-and-reboot). Older firmware keeps saving blocked
until storage returns to Pico; self-test and transition gates still apply.

USB ownership transitions leave CDC, HID and MSC connected; Pico-owned storage reports no medium through MSC.
See the [storage lifecycle](../../docs/design/architecture.md#storage-ownership-lifecycle).
For reboot or older firmware's ownership reconnects, the dashboard identifies a reconnect candidate by reading
Feature 1 and matching its stable device UID before resuming; it never replays the prior command.
**Return to Pico** is explicitly confirmed force return, not an OS eject.
**Enable USB drive** first opens a matching storage-handoff dialog explaining exclusive host access and safe eject;
Cancel or Escape sends no command, and confirmation rechecks the current safety gates.
Stop host file activity and safely eject whenever possible first. See the
[force-return risks](../../docs/design/usb/usb-hid-report-reference.md#msc-force-return-safety).
The card is not automatically formatted or repaired, and reconnect does not prove successful remount.

Firmware mounts at startup, keeps healthy firmware-owned storage mounted, and automatically remounts after normal
safe USB return; see the [storage lifecycle](../../docs/design/architecture.md#storage-ownership-lifecycle).
Sharing transfers exclusive access to the host; neither firmware files nor self-test are available then.
The optional self-test requires fresh, known self-test status and an already-mounted filesystem.
Start authorizes temporary `/PFTEST.BIN` writes, not mounting; an existing file is rejected, never replaced.
Preflight free-space scanning can delay acceptance before progress appears. Wait for the matching command result
instead of clicking Start again. Cleanup leaves the filesystem mounted.

## Checks

```sh
node --test host/webhid/tests/*.test.mjs
```

These are host-only protocol, transaction, freshness, and UI checks. Physical WebHID/USB behavior still requires
compatible firmware and RP2040/RP2350 hardware qualification.