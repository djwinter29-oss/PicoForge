# Host Tools Development

PicoForge has Python and .NET dashboards that share the frontend in `host/web`, plus an independent browser-only
WebHID dashboard in `host/webhid`. For installation and everyday use, see the
[Python host guide](../../host/python/README.md), [.NET host guide](../../host/dotnet/PicoForge/README.md), and
[WebHID guide](../../host/webhid/README.md).

Run commands from the repository root. Host unit tests do not require a board; physical device checks require an
identified board and appropriate USB permissions.

## Prerequisites

- Python 3.10+ for the Python host and repository contracts
- .NET 10 SDK for the .NET host and tests
- Node.js with the built-in test runner for WebHID tests
- A WebHID-capable desktop browser for physical browser use

Install only the toolchain needed for the host or checks you are changing.

## Run a Host Application

The launchers start the respective dashboard:

```sh
tools/host/run-python.sh
tools/host/run-dotnet.sh
tools/host/run-webhid.sh
```

Python and .NET serve the shared dashboard at `http://127.0.0.1:5000/` by default; run only one server on that port at
a time. WebHID serves the standalone browser app at `http://localhost:8000/`. A browser dashboard requires WebHID
support and local USB permission. The CDC console is documented in the [CDC CLI guide](../design/usb/cdc-cli.md).

The Python launcher provisions its environment on first use. To install the package explicitly for development and
tests:

```sh
python3 -m pip install --editable host/python
```

On Linux, HID access can require the repository's
[permission setup script](../../tools/host/hid-permissions-setup.sh), followed by reconnecting the device.

## Run Host Tests

Install the Python package first, then run the three hardware-free host suites together:

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```

The wrapper runs Python `unittest`, WebHID Node tests, and .NET tests. They can also be run individually:

```sh
python3 -m unittest discover -s host/python/tests -v
node --test host/webhid/tests/*.test.mjs
dotnet test host/dotnet/PicoForge.sln --configuration Release --nologo
```

Python tests cover the package and local web API; .NET tests cover the protocol and host behavior; WebHID tests cover
browser-side decoding and presentation. None opens a physical USB device. Repository-wide Python contracts are separate:

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests -m contracts
```

See the [test ownership index](../../tests/README.md) for cross-project checks and their prerequisites.

## Shared Frontend and Protocol Boundaries

Python and .NET serve the same assets from `host/web`; keep shared UI behavior backend-neutral and preserve the common
API response contract. WebHID is independent and owns its renderer and assets under `host/webhid`. Keep browser-specific
USB handling there rather than creating a second Python/.NET frontend.

Current host behavior follows the firmware's implemented HID reports. The
[HID Report Reference](../design/usb/hid-report-reference.md) describes a proposed consolidated protocol and is not
yet implemented; do not treat its report IDs or fields as the current host contract.

Only one active HID monitor per board should be used at a time. Separate host processes do not share device readers
and can conflict for access to the same USB interface.
