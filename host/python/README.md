# PicoForge Python Host

USB HID control, diagnostics, and a local dashboard for PicoForge firmware on Pico, Pico 2, and their W variants.
Basic monitoring needs no SD card. Storage and wireless controls depend on the connected firmware's capabilities.

Python and .NET share their own frontend in `host/web/src`. The Direct dashboard is independent: **no code is shared
with Direct**. See the
[Direct independence policy](../../docs/development/host-tools.md#direct-dashboard-independence).
This Python host connects over USB HID, not Bluetooth LE.
Wireless status uses Feature 12 and all writes use the unified Feature 20 channel. The dashboard preserves saved
service switches from Feature 2 and supports staged credentials, testing, scans, and USB-generated TCP CLI proofs.
Opaque scan SSIDs include `ssid_hex` beside an escaped display name; test/save requests may send that hex value
to preserve the network bytes. Saving a tested pair reuses its staging without resending either credential.
See the [authoritative wire contract](../../docs/reference/usb-hid.md) for layouts and expiry rules.

## Install

Use Python 3.10 or newer in a virtual environment:

```sh
python -m venv .venv
```

Activate it with `source .venv/bin/activate` on Linux/macOS or `.venv\Scripts\Activate.ps1` in Windows PowerShell, then:

```sh
python -m pip install picoforge
picoforge web
```

Open **http://127.0.0.1:5000/**. The wheel includes the local dashboard assets; Node.js and .NET are not required.
Runtime dependencies are Flask and HIDAPI, with versions defined in [pyproject.toml](pyproject.toml).
Native HID access still requires the operating system's usual libraries, drivers, and permissions.
The displayed application version comes from the installed `picoforge` distribution metadata when the imported
package is inside that metadata's distribution root, including source trees with local package metadata.
Without matching metadata, imports use `0.0.0-debug`; an unrelated installation does not override the fallback.
Release packaging stamps both distribution metadata and the source fallback.

Packages are published to [PyPI](https://pypi.org/project/picoforge/) and
[GitHub Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest) after successful
[release setup and publication](../../docs/deployment/release.md#publishing-setup); downloads may be unavailable before then.
For firmware installation and wiring, start with [Getting Started](../../docs/guides/getting-started.md).

### Run from a source checkout

From the repository root:

```sh
tools/host/run-python.sh
```

The launcher creates `build/picoforge` and installs the package on first use. Set `PYTHON` to choose the base
interpreter or `VENV_DIR` to choose its environment. For an explicit development installation in your active environment:

```sh
python -m pip install --editable host/python
```

## Commands

```sh
picoforge web
picoforge web --port 5001
picoforge status
picoforge status --serial SERIAL
picoforge reboot --serial SERIAL --confirm
picoforge --help
python -m picoforge --help
```

`status` prints JSON diagnostics. `reboot` requires explicit confirmation and uses the board's saved settings.
All three commands accept either `--serial SERIAL` or `--device-path PATH` to select a board.
The source launcher accepts the same command and options, for example `tools/host/run-python.sh web --port 5001`.
`status` returns the reports received within a five-second collection window; unavailable report fields remain `null`.
The top-level `msc` status key is retained for compatibility and deprecated in favor of `memory.msc`.

Use **Rescan** and the dashboard's board selector to switch between attached boards. Run only one HID host connection
per board; separate host processes can conflict for access. Python and .NET both default to port 5000, so use a
different port when running both.

On Linux, if HID access is denied, run [the permission setup script](../../tools/host/hid-permissions-setup.sh) from
the source checkout, reconnect the board, and restart the dashboard:

```sh
tools/host/hid-permissions-setup.sh
```

## Dashboard Operations

- **Monitor:** live board identity, runtime, memory, pins, SD-card details, and board LED control.
- **Boot configuration:** save clock, storage-speed, and supported radio settings with **Save&Reboot**.
  **Reboot Board** uses the already saved settings. See [settings and save safety](../../docs/reference/settings.md).
- **SD storage:** **Enable USB drive** hands the card to the computer; firmware file operations and self-tests are
  unavailable while USB owns it. Safely eject before returning it. **Return to Pico** requires data-loss confirmation;
  successful remount does not prove filesystem integrity. See
  [storage setup and use](../../docs/guides/getting-started.md#5-try-sd-card-storage) and
  [force-return risks](../../docs/reference/usb-hid.md#msc-force-return-safety).
- **SD benchmark:** start/cancel the firmware self-test and view verified bytes and throughput. It writes a temporary
  file: back up card data and follow the [benchmark procedure](../../docs/guides/sdcard-benchmark.md).
- **W-board wireless:** provision Wi-Fi and a Control key over USB. Bluetooth connections use the independent
  Direct dashboard. See [wireless setup and security limits](../../docs/guides/wireless.md).
- **Wi-Fi console:** a connected SSID/IP link opens the local TCP console when supported by firmware. Each dashboard
  process allows one active console session, bound to its selected board. The board's TCP service can require its saved
  Control key, but traffic is always unencrypted; use only on a trusted LAN. See
  [console setup and session limits](../../docs/guides/console.md#wi-fi-tcp-terminal).

Commands can be rejected or remain uncertain after a timeout/disconnect; delivery is not completion.
Do not replay uncertain writes. Feature availability and safety gates follow the connected firmware.
Builds and host tests do not establish physical hardware reliability; see the
[qualification gaps](../../docs/validation/qualification-gaps.md).

## Development and License

For host tests and frontend ownership, see [Host Tools Development](../../docs/development/host-tools.md). Detailed wire
reports and local wireless API semantics belong in the [USB HID reference](../../docs/reference/usb-hid.md) and
[wireless integration](../../docs/development/wireless-integration.md).

The package uses the **BSD-3-Clause** license. Its [LICENSE](LICENSE) mirrors the
[repository license](../../LICENSE) and is included in built wheels; keep both copies identical.
Package metadata, dependencies, and the `picoforge` entry point are defined in [pyproject.toml](pyproject.toml).

Import report codecs from `picoforge.reports`, HID access from `picoforge.hid`, and dashboard services from
`picoforge.dashboard`. Each public package can be imported independently without opening a device.
For module ownership, cleanup failure policy, and focused type checks, see
[Python host design](../../docs/development/host-tools.md#python-host-design).
