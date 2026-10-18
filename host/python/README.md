# PicoForge Python Tools

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard. The dashboard and `status` command include SD-card presence, transport, bus speed, capacity, and CID identity when available. From the repository root, `tools/host/run-python.sh` creates `build/picoforge`, installs this package on first run, and starts the dashboard. Pass `status` or `web` to select a command. Set `PYTHON` to choose the base interpreter or `VENV_DIR` to choose the environment path.

The dashboard binds to `127.0.0.1:8765`. Use `--serial SERIAL` or `--device-path PATH` when more than one board is connected. On Linux, if the launcher reports a permission error, run `tools/host/hid-permissions-setup.sh` and reconnect the Pico. The rule grants access to `plugdev` and the active desktop session.

The shared Python/.NET page refreshes automatically. Its first summary row includes the firmware release version
(`Unknown` for legacy firmware). LED control is in Live Report; USB-drive control is beside the SD-card self-test.
Selecting Start authorizes self-test writes and replacement of the reserved test file without a separate checkbox.

Use **Enable USB drive** to hand selected SD/NOR storage to the host. Firmware unmounts its filesystem before USB
briefly re-enumerates with an added MSC interface, keeping the HID identity and interface stable.

On Linux, **Disable USB drive** automatically unmounts the matching Pico's filesystems through UDisks2, requests
safe SCSI eject, then sends HID disable to return storage to Pico firmware. Disk selection checks the physical USB
VID/PID and the connected board's serial number; unrelated disks are never selected. This requires `lsblk`,
`udisksctl`, `gdbus`, a running UDisks2 service, and permission to unmount/eject that drive. The server never uses
sudo, force-unmount, or drive power-off. A busy filesystem or authorization failure stops the handoff and appears
on the page; close open files or safely eject using the host's disk manager and retry. Some partitions may already
be unmounted if a later unmount/eject fails, but HID disable is not sent on failure.

On other operating systems, and with the .NET or browser-only WebHID hosts, safely eject in the operating system
before disabling. Browsers cannot unmount OS filesystems. The firmware's safe-eject requirement is unchanged.

The separate **Force return to Pico** action requires data-loss confirmation and bypasses host eject, not
device sync. It is available only with supporting firmware. See the authoritative
[force-return contract and risks](../../docs/usb-protocol.md#confirmed-force-return); a successful remount
does not establish filesystem integrity.

The dashboard also controls the [SD-card self-test](../../docs/sd-self-test.md)
over HID and displays progress, verified byte counts, and average MiB/s. Its
`POST /api/self-test` accepts `{"action":"start","size_mib":32}` or
`{"action":"cancel"}`. A 202 response acknowledges delivery; completion and
firmware rejection come from `self_test` in `/api/status`.

Run all hardware-free host unit suites from the repository root (install this package first):

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```
