# PicoForge Python Host

The Python host reads the PicoForge vendor-defined HID interface and can serve a local status dashboard.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
python -m picoforge_host status
python -m picoforge_host web
```

The dashboard binds to `127.0.0.1:8765`. Use `--serial SERIAL` or `--device-path PATH` when more than one board is connected. On Linux, grant the user access to the matching HID interface before connecting.

Run the hardware-free protocol tests from the repository root:

```sh
PYTHONPATH=host/python/src python3 -m unittest discover -s host/python/tests -v
```