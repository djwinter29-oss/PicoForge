# PicoForge Python Tools

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard. The dashboard and `status` command include SD-card presence, transport, bus speed, capacity, and CID identity when available. From the repository root, `tools/host/run-python.sh` creates `build/picoforge`, installs this package on first run, and starts the dashboard. Pass `status` or `web` to select a command. Set `PYTHON` to choose the base interpreter or `VENV_DIR` to choose the environment path.

The dashboard binds to `127.0.0.1:8765`. Use `--serial SERIAL` or `--device-path PATH` when more than one board is connected. On Linux, if the launcher reports a permission error, run `tools/host/hid-permissions-setup.sh` and reconnect the Pico. The rule grants access to `plugdev` and the active desktop session.

Use **Enable USB drive** to hand the SD card to the host; USB briefly re-enumerates with an added MSC interface while keeping the HID identity and interface stable. Safely eject it in the host operating system before disabling MSC; the Pico then regains card ownership. Firmware storage must be unmounted before enabling host ownership.

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
