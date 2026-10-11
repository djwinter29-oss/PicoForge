# PicoForge Python Tools

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
python -m picoforge status
python -m picoforge web
```

The dashboard binds to `127.0.0.1:8765`. Use `--serial SERIAL` or `--device-path PATH` when more than one board is connected. On Linux, `tools/host/run-python.sh` installs the included udev rule if needed (sudo may prompt), reloads udev, and triggers HID device permissions before launch. Reconnect the board if the active session does not gain access immediately.

On Linux, install the included udev rule once, then reconnect the board so the active desktop user can open its HID interface:

```sh
sudo install -m 0644 tools/host/99-picoforge-hid.rules /etc/udev/rules.d/99-picoforge-hid.rules
sudo udevadm control --reload-rules
```

Unplug and reconnect the Pico after reloading the rules. The rule uses the `uaccess` tag for the logged-in desktop session.

Run all hardware-free host unit suites from the repository root (install this package first):

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```
