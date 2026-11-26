# PicoForge Direct Dashboard

[Documentation](../../docs/README.md) / [Dashboard guides](../../docs/guides/README.md#dashboards)

Connect to a PicoForge board directly from desktop Chrome or Edge using **USB (WebHID)** or
**Bluetooth LE (Web Bluetooth)**. View live diagnostics, edit boot settings, and run supported board controls
without installing the Python or .NET host application.

The optional Wi-Fi CLI console requires the included local server. The dashboard itself uses plain browser
ES modules: no bundle, build step, or runtime JavaScript package installation is required.

- [Quick start](#quick-start)
- [Dashboard layout](#dashboard-layout)
- [Controls](#controls)
- [Page lifecycle](#page-lifecycle)
- [Connection behavior](#connection-behavior)
- [Development](#development)
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

On Linux, if HID access is denied, run the [HID permission setup script](../../tools/host/hid-permissions-setup.sh)
from a source checkout, reconnect the board, then restart the dashboard:

```sh
tools/host/hid-permissions-setup.sh
```

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
[console guide](../../docs/guides/console.md#wi-fi-tcp-terminal) for authentication and security limits.

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
Commands and terminal input are never automatically replayed. Pending-operation recovery uses temporary data in
the current tab; keep the tab open while resolving an uncertain result. Ending the tab discards that recovery
context. Recovery data does not contain SSIDs, passwords, or Control keys.

The Wi-Fi expiry countdown updates locally once per second and notifies Save readiness without waiting for another
board report. Expired credentials must be tested again.

## Connection behavior

The browser enables controls only after validating identity, configuration, and fresh status. Timed-out or disconnected
commands are not replayed; resolve uncertain outcomes before continuing. USB framing, capabilities, result
matching, and Input-report freshness are defined by the [HID reference](../../docs/reference/usb-hid.md).
The [BLE reference](../../docs/reference/bluetooth.md) owns Bluetooth framing and authorization. Direct's
[wireless polling policy](../../docs/development/direct-dashboard.md#polling-and-freshness) is client-specific.

## Development

For source layout, unit/type/browser checks, performance methodology, and hardware qualification, use
[Direct dashboard development](../../docs/development/direct-dashboard.md). This guide covers installation and use.

## Further reading

- [Wireless setup](../../docs/guides/wireless.md)
- [Console authentication and security limits](../../docs/guides/console.md#wi-fi-tcp-terminal)
- [HID report reference](../../docs/reference/usb-hid.md)
- [Storage lifecycle](../../docs/reference/architecture.md#storage-ownership-lifecycle)
- [Forced-save policy](../../docs/reference/settings.md#forced-save-and-reboot)
- [Host development and Direct independence](../../docs/development/host-tools.md#direct-dashboard-independence)
- [Hardware qualification gaps](../../docs/evidence/README.md)
- [Release and Pages publishing](../../docs/development/releases.md#publishing-setup)
