# PicoForge Python Tools

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard. The dashboard and `status` command include SD-card presence, transport, bus speed, capacity, and CID identity when available. From the repository root, `tools/host/run-python.sh` creates `build/picoforge`, installs this package on first run, and starts the dashboard. Pass `status` or `web` to select a command. Set `PYTHON` to choose the base interpreter or `VENV_DIR` to choose the environment path.

The dashboard defaults to `127.0.0.1:5000`. Use **Rescan** and the board selector in Device identity to switch between attached boards; `--serial SERIAL` or `--device-path PATH` can set the preferred initial selection. Override the port with `tools/host/run-python.sh web --port PORT`. Only one dashboard server can bind to port 5000 at a time. On Linux, if the launcher reports a permission error, run `tools/host/hid-permissions-setup.sh` and reconnect the Pico. The rule grants access to `plugdev` and the active desktop session.

The shared Python/.NET page refreshes automatically. Its Device information row follows Pico UART's field order:
product, board model, serial / UID, system clock, firmware RAM reserved / total, and firmware version
(`Unknown` for legacy firmware). The six fields share one desktop row and wrap on smaller screens.
The **Board LED** toggle is in Device information; SD sharing is beside the self-test.
The **Pin configuration** disclosure under Board configuration is collapsed by default; expand it to view
monitor UART pins and storage GPIOs. Reset cause stays visible beside the clock and SD-card settings.
Selecting Start authorizes temporary self-test writes. An existing `/PFTEST.BIN` file or directory is rejected and
never replaced.

**Save and Reboot** asks for confirmation and sends one HID settings command. Firmware saves the settings and
queues its own reboot only on success; the host verifies the saved values in the command result without sending
a separate reboot command. **Reboot Board** confirms a reboot using the
already saved settings and warns when unsaved dashboard changes will be lost. Both use Pico UART-style modal
dialogs with Cancel and explicit action buttons; Escape cancels without sending a command.

Use **Share SD card** to hand the SD card to the host. Firmware unmounts its filesystem before USB
briefly re-enumerates with an added MSC interface, keeping the HID identity and interface stable.

**Return to Pico** requires data-loss confirmation, then requests Pico-side detach and remount over HID.
It uses the same styled confirmation dialog as reboot, with Cancel and Escape sending no command.
The same command is used on Windows, Linux and macOS; the host app never runs OS eject/unmount commands and
requires no UDisks2 or eject permissions. The button requires supporting firmware; older firmware shows an
update message instead of silently falling back to a different behavior. See the authoritative
[force-return design and risks](../../docs/design/usb/usb-hid-report-reference.md#msc-force-return-safety); a successful remount
does not establish filesystem integrity. This is OS-independent command behavior, not physical qualification
on every OS. Native HID access still requires the platform's usual permissions and drivers.

The dashboard also controls the [SD-card self-test](../../docs/design/sdcard/sdcard-performance-test.md)
over HID and displays progress, verified byte counts, and average MiB/s. Its
`POST /api/self-test` accepts `{"action":"start","size_mib":32}` or
`{"action":"cancel"}`. A 202 response acknowledges delivery; completion and
firmware rejection come from `self_test` in `/api/status`.

Run all hardware-free host unit suites from the repository root (install this package first):

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```
