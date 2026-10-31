# PicoForge Python Tools

## Install

With Python 3.10+ in a virtual environment:

```sh
python -m pip install picoforge
picoforge web
```

The [PyPI package](https://pypi.org/project/picoforge/) includes the local dashboard assets.
The same wheel is attached to [GitHub Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest).
These downloads become available after the initial [publishing setup](../../docs/deployment/release.md#publishing-setup)
and successful release. For source development, use `python -m pip install --editable host/python` at the repository root.

## Usage

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard. The dashboard and `status` command include SD-card presence, transport, bus speed, capacity, and CID identity when available. From the repository root, `tools/host/run-python.sh` creates `build/picoforge`, installs this package on first run, and starts the dashboard. Pass `status` or `web` to select a command. Set `PYTHON` to choose the base interpreter or `VENV_DIR` to choose the environment path.

The dashboard defaults to `127.0.0.1:5000`. Use **Rescan** and the board selector in Device identity to switch between attached boards; `--serial SERIAL` or `--device-path PATH` can set the preferred initial selection. Override the port with `tools/host/run-python.sh web --port PORT`. Only one dashboard server can bind to port 5000 at a time. On Linux, if the launcher reports a permission error, run `tools/host/hid-permissions-setup.sh` and reconnect the Pico. The rule grants access to `plugdev` and the active desktop session.

The shared Python/.NET page refreshes automatically. Its Device information row follows Pico UART's field order:
product, board model, serial / UID, system clock, firmware RAM reserved / total, and firmware version
(`Unknown` for legacy firmware). The six fields share one desktop row and wrap on smaller screens.
The **Board LED** toggle is in Device information; SD sharing is beside the self-test.
The **Pin configuration** disclosure under Board configuration is collapsed by default; expand it to view
separate Monitor UART and Storage tables, each with signal names across the first row and GPIO values across
the second row, including card detect. Monitor UART appears above Storage, with both tables spanning the area.
Reset cause stays visible beside the clock and SD-card settings.
During benchmark I/O, the storage status sampler may be unable to acquire the driver lock. The dashboard shows
**Busy**, not an initialization failure, and retains the last sampled card details without adding a message above
the table, keeping the layout stable during I/O.
Retained details are cleared on disconnect, board selection changes, stale reports, ownership transitions, or a
fresh sample showing absent or uninitialized media.
The [storage lifecycle](../../docs/design/architecture.md#storage-ownership-lifecycle) mounts at startup and keeps
healthy firmware-owned media mounted; normal safe USB return remounts automatically. Wait for fresh mounted status.
Selecting Start authorizes temporary self-test writes, not a mount-only step. Preflight free-space scanning can
delay acceptance before progress appears; wait for its command result rather than clicking again.
An existing `/PFTEST.BIN` file or directory is rejected and never replaced. Test cleanup leaves storage mounted.

**Save and Reboot** asks for confirmation and sends one HID settings command. Firmware saves the settings and
queues its own reboot only on success; the host verifies the saved values in the command result without sending
a separate reboot command. **Reboot Board** confirms a reboot using the
already saved settings and warns when unsaved dashboard changes will be lost. Both use Pico UART-style modal
dialogs with Cancel and explicit action buttons; Escape cancels without sending a command.
Local settings edits are also available while USB owns the card. Supporting firmware enables **Save and Reboot**
with an additional host-cache data-loss warning and explicit forced-save confirmation. Device sync and USB detach
must succeed before saving; self-tests and transitions still block saving. See the
[forced-save safety policy](../../docs/design/settings.md#forced-save-and-reboot). Older firmware requires returning
storage first; host commands are never silently downgraded or replayed after reconnection.

Use **Enable USB drive** and confirm the storage-handoff dialog to hand the SD card to the host.
Cancel or Escape sends no command. Firmware unmounts its filesystem before making the MSC medium available;
CDC and HID stay connected. The interface remains present as an empty removable drive while Pico owns the card.
See the [storage lifecycle](../../docs/design/architecture.md#storage-ownership-lifecycle).
With older firmware, after a confirmed ownership-change command the dashboard shows **Reconnecting** for expected HID disconnects
for up to five seconds. Errors that persist beyond that window, invalid reports, and uncertain command outcomes
remain errors; the reconnect notice does not confirm successful storage handoff.
Firmware file operations and self-test remain unavailable until USB ownership returns and mounting succeeds.

**Return to Pico** requires data-loss confirmation, then requests Pico-side host-access revocation and remount over HID.
It uses the same styled confirmation dialog as reboot, with Cancel and Escape sending no command.
The same command is used on Windows, Linux and macOS; the host app never runs OS eject/unmount commands and
requires no UDisks2 or eject permissions. The button requires supporting firmware; older firmware shows an
update message instead of silently falling back to a different behavior. See the authoritative
[force-return design and risks](../../docs/design/usb/usb-hid-report-reference.md#msc-force-return-safety); a successful remount
does not establish filesystem integrity. This is OS-independent command behavior, not physical qualification
on every OS. Native HID access still requires the platform's usual permissions and drivers.

The dashboard also controls the [SD-card self-test](../../docs/guides/sdcard-performance-test.md)
over HID and displays progress, verified byte counts, and average MiB/s. Its
`POST /api/self-test` accepts `{"action":"start","size_mib":32}` or
`{"action":"cancel"}`. A 202 response acknowledges delivery; completion and
firmware rejection come from `self_test` in `/api/status`.

USB wireless-capable firmware exposes sanitized configuration through `GET /api/wireless?board=ID`.
`POST /api/wireless?board=ID` requires boolean `wifi_enabled`, `ble_enabled`, `confirm_reboot: true`,
and `ssid` (at most 32 UTF-8 bytes; nonempty when Wi-Fi is enabled). Optional `password` must contain
8..63 printable ASCII characters; optional `ble_key` must contain 64 hexadecimal characters and be nonzero.
Omitting either credential preserves its stored value. Responses never contain credentials.
Saving requires idle, firmware-owned storage and no active self-test or outstanding command.
The host sends Feature 31 once and verifies its transaction, saved flags, SSID, and credential-presence booleans
using Feature 30 before marking reboot pending. A mismatched saved configuration leaves the outcome uncertain.
Timeouts or disconnects leave the outcome uncertain and block further writes; commands are not replayed.
The local dashboards expose **Wi-Fi and Bluetooth** settings for the selected USB-attached W board, including
SSID, password and a generated BLE control key. See the
[wireless configuration and security policy](../../docs/design/wireless.md).
Bluetooth LE connections remain available only from the standalone WebHID page.

Run all hardware-free host unit suites from the repository root (install this package first):

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```
