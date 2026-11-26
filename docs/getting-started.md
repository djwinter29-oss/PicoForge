# Getting Started with PicoForge

[Documentation](README.md)

Set up firmware and a host connection, then try board monitoring and optional SD-card storage.

## Before You Connect

- GPIO and SD-card signals are **3.3 V only**. Never apply 5 V or RS-232 levels directly; use a common ground.
- Mount only cards with data you have backed up. PicoForge does not format a card during automatic mounting.
- Do not access files from the computer and firmware simultaneously. Safely eject before disabling USB mass storage.
- Do not unplug or remove a card during writes or a self-test.
- High-speed SD-card operation depends on the board, card, breakout, wiring, and clock. A short successful test does
  not guarantee sustained reliability.
- Physical USB mass-storage workflows and power-loss recovery are not fully qualified. See the
  [test records](evidence/records/README.md) for the scope and limits of existing measurements.

## 1. Get or Build the Firmware

### Download and verify

Download the firmware package for your board and SD transport from
[Releases](https://github.com/djwinter29-oss/PicoForge/releases/latest). Download `SHA256SUMS.txt` from the same
release and verify the archive before extracting it. On Linux, with both files in the same directory:

```sh
sha256sum --check SHA256SUMS.txt --ignore-missing
```

The same check applies to downloaded host archives. If packages are not yet published, build from source.

### Build from source

Install Git with submodule support, CMake, Ninja, and `arm-none-eabi-gcc`.

Clone the repository and enter the checkout:

```sh
git clone https://github.com/djwinter29-oss/PicoForge.git
cd PicoForge
```

From the repository root:

```sh
git submodule update --init --recursive
tools/firmware/build.sh --board pico2 --sdcard-transport SDIO_PIO
```

Choose `--board pico` for the original Pico and `--board pico2` for Pico 2. The build script also accepts `pico_w`
and `pico2_w`; use firmware that matches your board. Choose `--sdcard-transport SPI` or `SDIO_PIO` to match your
breakout wiring. Transport is selected at build time, not when connecting a dashboard.
Add `--benchmark OFF` or `--wireless OFF` to leave out
[optional modules](reference/build-configuration.md#optional-modules).

The build creates a separate directory for each board/transport pair, such as `build/firmware-pico2-SDIO_PIO/`.
Stock-clock Pico 2 builds produce a dated image named `pico_forge_rp2350_150_<YYYYMMDD>.uf2`; Pico builds use
`pico_forge_rp2040_125_<YYYYMMDD>.uf2`. For component defaults and supported combinations, see
[build configuration](reference/build-configuration.md).
Select clock and SD-speed profiles through the [dashboard settings](reference/settings.md#host-dashboard-settings),
not build options.

## 2. Flash the Board

1. Hold **BOOTSEL** while connecting the board to your computer over USB.
2. Copy the `.uf2` built for your board to the USB drive that appears.
3. The board restarts automatically.

## 3. Connect the Hardware

Basic monitoring works without SD-card hardware. For storage, wire a compatible 3.3 V SD-card breakout and card
using the [hardware setup guide](guides/hardware.md). Verify the breakout pinout and board connections before
enabling storage.

The hardware guide also covers independent monitor UART logging and SWD debugging. Monitor UART wiring follows
the [PicoUart board setup](https://github.com/djwinter29-oss/PicoUart/blob/main/docs/hardware-setup.md) approach,
but PicoForge is not a multi-port UART bridge; use USB CDC for the command console.

## 4. Choose a Dashboard or Console

Open the hosted [Direct dashboard](https://djwinter29-oss.github.io/PicoForge/) in desktop Chrome or Edge.
Connect the board to the computer running the browser, choose **USB**, then **Select board**. W boards can also be
monitored over **Bluetooth** after USB provisioning. No host installation is needed. See the
[Direct dashboard guide](../host/direct/README.md) for local hosting and connection details.

For local Python/.NET apps and their prerequisites, [choose a dashboard](guides/README.md#dashboards).
For a serial terminal, open the PicoForge USB CDC port and enter `help`; use the
[console guide](guides/console.md#terminal), not the Debug Probe UART.

Use only one dashboard/HID monitor per board. If access fails, follow your chosen dashboard's permissions and
connection troubleshooting instructions.

Before using controls, confirm that the dashboard identifies the intended board/MCU and serial and shows advancing
uptime with fresh status. An absent SD card is expected when no storage hardware is connected.

## 5. Try SD-Card Storage

With a correctly wired card, firmware attempts to mount its existing FAT/exFAT filesystem automatically at startup,
without formatting it. Wait for fresh `mounted` status before file operations.

Continue with [SD-card storage](guides/storage.md) to read files, share the card with your computer, or remove it
safely. Benchmarking writes to the card; read the [benchmark procedure](guides/sdcard-benchmark.md) before starting.
