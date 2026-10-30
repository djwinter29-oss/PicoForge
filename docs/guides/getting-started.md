# Getting Started with PicoForge

Set up firmware and a host connection, then try board monitoring and optional SD-card storage.
For the project overview and documentation index, see the [README](../../README.md).

## Before You Connect

- GPIO and SD-card signals are **3.3 V only**. Never apply 5 V or RS-232 levels directly; use a common ground.
- Mount only cards with data you have backed up. PicoForge does not format a card during automatic mounting.
- Do not access files from the computer and firmware simultaneously. Safely eject before disabling USB mass storage.
- Do not unplug or remove a card during writes or a self-test.
- High-speed SD-card operation depends on the board, card, breakout, wiring, and clock. A short successful test does
  not guarantee sustained reliability.
- Physical USB mass-storage workflows and power-loss recovery are not fully qualified. See the
  [test records](../tests/records/README.md) for the scope and limits of existing measurements.

## 1. Get or Build the Firmware

Download the firmware package for your board and SD transport from
[Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest), or build from a source checkout. Firmware
builds require Git with submodule support, CMake, Ninja, and `arm-none-eabi-gcc`.

From the repository root:

```sh
git submodule update --init --recursive
tools/firmware/build.sh --board pico2 --sdcard-transport SDIO_PIO
```

Choose `--board pico` for the original Pico and `--board pico2` for Pico 2. The build script also accepts `pico_w`
and `pico2_w`; use firmware that matches your board. Choose `--sdcard-transport SPI` or `SDIO_PIO` to match your
breakout wiring. Transport is selected at build time, not when connecting a dashboard.

The build creates a separate directory for each board/transport pair, such as `build/firmware-pico2-SDIO_PIO/`.
Stock-clock Pico 2 builds produce a dated image named `pico_forge_rp2350_150_<YYYYMMDD>.uf2`; Pico builds use
`pico_forge_rp2040_125_<YYYYMMDD>.uf2`. See the [release guide](../deployment/release.md) for published packages and
build configurations. The build script enables the storage self-test; direct CMake builds disable it by default.

Publication links may be unavailable before the first successful publication. Maintainers should follow the
[release setup](../deployment/release.md#publishing-setup) before announcing downloads.

## 2. Flash the Board

1. Hold **BOOTSEL** while connecting the board to your computer over USB.
2. Copy the `.uf2` built for your board to the USB drive that appears.
3. The board restarts automatically.

## 3. Connect the Hardware

Basic monitoring works without SD-card hardware. For storage, wire a compatible 3.3 V SD-card breakout and card
using the [hardware setup guide](hardware-setup.md). Verify the breakout pinout and board connections before
enabling storage.

The hardware guide also covers independent monitor UART logging and SWD debugging. Monitor UART wiring follows
the [PicoUart board setup](https://github.com/djwinter29-oss/PicoUart/blob/main/docs/hardware-setup.md) approach,
but PicoForge is not a multi-port UART bridge; use USB CDC for the command console.

## 4. Choose a Dashboard or Console

### Browser Dashboard

Open the hosted [WebHID dashboard](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome or Edge.
Connect the board to the computer running the browser, click **Connect**, and select the board.
No host installation is needed. See the [WebHID guide](../../host/webhid/README.md) for local hosting and
connection details.

### Local Python Dashboard

With Python 3.10 or newer, install the package in a virtual environment:

```sh
python -m pip install picoforge
picoforge web
```

The dashboard opens at **http://127.0.0.1:5000/**. From a source checkout, `tools/host/run-python.sh` creates an
environment and installs dependencies on first use. See the [Python host guide](../../host/python/README.md).

### .NET Dashboard

Download a self-contained host from [Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest)
for Windows x64, Linux x64, or macOS ARM64. Extract the full archive and run `PicoForge.exe` on Windows or
`./PicoForge` on Linux/macOS, then open **http://127.0.0.1:5000/**.

Native HID dependencies and operating-system permissions still apply. From a source checkout, run
`tools/host/run-dotnet.sh` with the .NET 10 SDK. See the [.NET host guide](../../host/dotnet/PicoForge/README.md).

### USB Serial Console

Open the PicoForge USB CDC port in a serial terminal and enter `help` to list commands. Choose the PicoForge CDC
port, not a Debug Probe's UART port. See the [console guide](usb-cdc-cli.md) for terminal settings, commands,
and recovery behavior.

### Connection Troubleshooting

Python and .NET dashboards both default to port 5000; run one at a time or choose another port. Use only one
dashboard connection per board at a time. On Linux, if HID access is denied, run
`tools/host/hid-permissions-setup.sh`, then reconnect the board and restart the dashboard.

## 5. Try SD-Card Storage

With a correctly wired card, firmware attempts to mount its existing FAT/exFAT filesystem automatically at startup,
without opening a dashboard or starting a self-test. It stays mounted for firmware use unless explicitly unmounted
or shared over USB; see the [storage lifecycle](../design/architecture.md#storage-ownership-lifecycle).
Mounting never formats the card. Wait for `storage status` to report `mounted` before file operations:

```text
storage status
fs ls /
fs cat /example.txt
```

To share the card with your computer:

1. Finish file operations. You may enter `fs unmount` in the console; enabling USB mass storage also queues an unmount.
2. Enable USB mass storage (MSC) in the dashboard; USB briefly reconnects.
3. Use the card from your computer.
4. Safely eject the drive before returning ownership to PicoForge. The legacy safe-disable API is documented in the
   [HID reference](../design/usb/usb-hid-report-reference.md). The dashboard's **Return to Pico** action requests a
   confirmed Pico-side detach/remount, not an OS eject; read the
   [force-return risks](../design/usb/usb-hid-report-reference.md#msc-force-return-safety) before using it.
5. Wait for fresh mounted status before using files or starting a self-test. Normal safe return remounts automatically;
   do not click Start to mount the card. A failed forced-return mount requires inspection and deliberate `fs mount`.

On supporting firmware, the dashboard self-test writes and verifies `/PFTEST.BIN`; it refuses to overwrite an
existing file at that path. Starting a test authorizes card writes and contributes to card wear. Back up important
data first. Start runs the test against the existing mount, leaving it mounted afterward; checking free space can
delay acceptance before write progress appears. The test is not filesystem repair or a guarantee of card reliability.
Read the [self-test guide](sdcard-performance-test.md) for safeguards and result interpretation.
