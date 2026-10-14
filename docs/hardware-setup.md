# PicoForge Hardware Setup Options

Reference hardware setup for standard Pico/Pico 2 boards and external breakouts. The memory and storage pin map below lists the candidate GPIO allocation for each interface mode. Select one mode per connected device. Some options are not implemented in firmware. `TBD` means unassigned.

## Board and Build Selection

| Item | Selection |
| --- | --- |
| Reference board | Raspberry Pi Pico, standard 40-pin header |
| Alternate build target | Raspberry Pi Pico 2 (`PICO_BOARD=pico2`); the firmware build defaults to `pico` |
| MCU | RP2040 on Pico; RP2350 on Pico 2 |
| Reference power | USB through the board's USB connector |
| Optional overclock | `tools/firmware/build.sh --board pico2 --sys-clk-mhz 200` (or `PF_SYS_CLK_MHZ=200` with `load.sh`); out of spec, range 100-250 MHz, default `0` keeps the SDK clock. The SDK applies it at boot and raises the regulator to 1.15 V above 150 MHz and 1.30 V above 200 MHz. The SDIO board profile requests 20 MHz; at the stock 150 MHz system clock the measured bus is 18.75 MHz. |

## Debug and Monitoring

Serial and SWD do not overlap the memory pins below. Share ground with the adapter or probe; ground pins are omitted from the tables.

### Serial

| MCU GPIO | Header pin | UART0 |
| --- | --- | --- |
| GP0 | 1 | TX |
| GP1 | 2 | RX |

Connect MCU TX to adapter RX and MCU RX to adapter TX, and connect GND. Board support initializes the monitor UART from the selected topology; the Pico/Pico 2 profiles assign UART0 to GP0/GP1 at 115200 baud, 8-N-1. UART logging has been verified on a Pico through the Raspberry Pi Debug Probe. The firmware emits boot messages and a heartbeat each second, including uptime and system clock. On Linux the probe's UART bridge appears as a serial device (for example `/dev/ttyACM0`; device numbering can vary), while PicoForge USB CDC is a separate port (for example `/dev/ttyACM1`). Open the probe UART at 115200 baud, for example with `screen /dev/ttyACM0 115200`. Reset the Pico after opening the terminal to see boot messages. No command console or RTS/CTS flow control is implemented; text sent to UART RX is not processed. Use 3.3 V logic, never RS-232 levels. USB CDC remains a separate interface.

### SWD

| MCU pad | Header pin | SWD |
| --- | --- | --- |
| SWDIO debug pad | N/A | SWDIO |
| SWCLK debug pad | N/A | SWCLK |

Connect probe SWDIO/SWCLK to the matching board pads. Verify their labels on the selected board; these are not numbered pins on the 40-pin header.

## Memory and Storage Connections

Use GP2-GP7 for one memory device and choose one mode column below. GP7 is the CS pin in SD-SPI, QSPI-PIO, and SPI-NOR modes; it is controlled by GPIO/PIO, not automatic hardware CS. SDIO has no CS signal.

| MCU GPIO | Header pin | SDIO-PIO | SD-SPI (SPI0) | QSPI-PIO | SPI-NOR flash (SPI0) |
| --- | --- | --- | --- | --- | --- |
| GP2 | 4 | CLK | CLK / SCK | CLK | CLK / SCK |
| GP3 | 5 | CMD | SI / MOSI | IO0 | IO0 / MOSI |
| GP4 | 6 | DAT0 | SO / MISO | IO1 | IO1 / MISO |
| GP5 | 7 | DAT1 | - | IO2 | WP (hold high) |
| GP6 | 9 | DAT2 | - | IO3 | HOLD (hold high) |
| GP7 | 10 | DAT3 | CS (GPIO-controlled) | CS | CS (GPIO-controlled) |
| GP8 | 11 | DET (optional) | DET (optional) | - | - |

- `-` means unused in that mode, not necessarily disconnected. GP8 is optional SD-card detect.
- Use 3.3 V and common ground, never 5 V. Power off before replacing a breakout.
- SDIO-PIO requires logical DAT0-DAT3 on four ascending consecutive GPIOs, in that order; lane swaps are unsupported. The board profile assigns CLK=GP2, CMD=GP3, and DAT0-DAT3 to GP4/GP5/GP6/GP7. PIO samples the contiguous GP4-GP7 window directly.
- The SD-SPI driver holds GPIO-controlled CS low across each complete card transaction, including data and busy-response phases. Verify CS timing, clock, and data with a logic analyzer during board validation.
- SDIO has no CS signal. In SD-SPI mode, the driver controls the breakout's DAT3/CS line on GP7 as ordinary GPIO; no automatic hardware CS is used.
- Pin names follow the breakout guides below. Pico 2 SDIO and SPI filesystem CLI operations are verified as recorded below; other boards/interface modes and signal integrity have not been validated.

### Lab Hardware Examples

The lab uses two types of memory/storage breakout: the Adafruit microSD board and W25Q128 flash board described below. These are reference examples, not an exclusive list of compatible devices. Other SD-card or SPI/QSPI memory devices can use this pin allocation if their supply and logic levels, signal functions, pull-ups, and timing requirements are compatible. Check each device's pinout and protocol; firmware may need device-specific initialization or commands. The active-high DET behavior and WP/HOLD requirements below describe these particular lab boards and must be checked for any replacement. Use in the lab does not establish that all interface modes have been validated.

#### SD-Card Breakout

The selected module is Adafruit's [Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its [pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Wire DAT0/DAT1/DAT2/DAT3-CS to GP4/GP5/GP6/GP7 respectively in both modes. DAT1 and DAT2 are unused inputs in SD-SPI mode; GP7 drives CS in that mode. Board support can construct either transport configuration and sets its active-high DET signal on GP8 using the breakout's pull-up, without an MCU pull. Both drivers explicitly disable inherited MCU pulls when the external DET pull-up is selected. The breakout and driver both provide SDIO data pull-ups, so check their combined strength. Startup attempts card initialization once but does not mount a filesystem. SPI requests use single-block commands for each sector. Mode changes require protocol reinitialization. When switching from SPI to native SDIO, fully remove card power: an MCU reset alone can leave the card latched in SPI mode. Fixed wiring does not imply hot switching.

Pico 2 SDIO initialization was verified with the connected 16 GB card: UART reported `SD card initialized`, and HID reported `initialized=true`, 30,875,648 sectors (15,808,331,776 bytes), and a 12.5 MHz bus clock. The command PIO now drives all 48 command bits before rising clock edges and captures the response start bit without a software bit shift. Command completion allows at least eight idle clock cycles before the next command. That initial check did not exercise sector I/O or filesystem mounting and wrote no card data; the full SDIO CLI qualification is recorded below.

On 2026-10-13, the same Pico 2 and card were verified in SPI mode at 25 MHz after reboot. HID reported the same capacity and CID. The SPI driver explicitly pulls MISO high so released responses have a defined idle level; this test does not isolate that change from the reboot as the cause of successful initialization. The initial read-only CDC CLI check successfully mounted the existing filesystem, listed `/`, read directory metadata with `fs stat`, and reported total/free filesystem space with `fs df` (15,799,943,168 / 15,799,877,632 bytes). These operations exercised SPI sector reads and CRC checks. HID identity and runtime/memory/MSC input reports remained accessible during CDC use. The filesystem was explicitly unmounted afterward, with MSC disabled and Pico retaining card ownership. No files were changed or formatted during that initial check. SPI was explicitly selected for this qualification; select the intended transport before building.

A subsequent authorized CDC CLI qualification on the same date passed 36 checks using only disposable files in `/pf-cli-test-e700f283`. Verified commands were `help fs`, `sd status`, `fs mount`, `unmount`, `ls`, `stat`, bounded/default `cat`, `df`, `mkdir`, `write`, `append`, `mv`, and confirmed `rm`. Writing and appending produced the exact expected readback (`hello world more text`, 21 bytes). Guard checks refused operations before mount, duplicate creation, write/rename overwrites, unsafe paths, invalid read limits, unconfirmed deletion, and directory deletion. HID identity and runtime/memory/MSC reports continued throughout. Both test files were deleted and their absence verified; only the empty test directory remains. The filesystem was unmounted, MSC disabled, and Pico retained ownership. Existing files were not changed and media was not formatted. This verifies the tested small-file SPI workflow, not sustained-write endurance, power-loss recovery, detailed backpressure, or the SPI MSC handoff lifecycle.

The same 36-check suite subsequently passed in native SDIO mode at 12.5 MHz on
2026-10-13. Switching from SPI first failed CMD8 after a warm MCU reset; a full
card power cycle restored native initialization. Initial directory creation then
failed with `DISK_ERR`. The write PIO had treated the CRC-status end bit as ready
before the card entered its busy phase, allowing a subsequent CMD24 too early.
The corrected program waits through the following falling/rising clock edges
before waiting for DAT0 to release busy. Write start/stop and bus release also
transition on falling edges. The focused directory write then succeeded, and
the full suite passed on clean firmware with temporary diagnostics removed.
Readback again matched `hello world more text` (21 bytes), all safety guards
passed, and HID identity/runtime/memory/MSC reports stayed available. Test files
were deleted and absence verified; empty `/pf-sdio-test-2efe1ac8` and diagnostic
`/pf-sdio-diag-12d12bef` directories remain alongside the earlier SPI test
directory. No existing file contents were changed and no formatting was done.
SDIO remains selected, the filesystem is unmounted, MSC is disabled, and Pico
owns the card. These checks do not qualify endurance, power-loss recovery,
high-speed timing margins, or complete SDIO MSC handoff workflows.

#### W25Q128 Flash Breakout

The [Adafruit QSPI DIP Breakout - W25Q128](https://www.adafruit.com/product/5634) is a 3.3 V, 16 MB serial NOR flash supporting QSPI and SPI; see its [pinout guide](https://learn.adafruit.com/adafruit-qspi-dip-breakout-boards/pinouts). Connect CLK, IO0, IO1, IO2, IO3, and CS to GP2, GP3, GP4, GP5, GP6, and GP7 respectively in both modes. This keeps IO0-IO3 on consecutive GP3-GP6 and shares CS on GP7 with the SD breakout. Use pull-ups on GP7/CS, GP5/IO2/WP, and GP6/IO3/HOLD to keep them high through reset; in SPI-NOR mode, keep GP5/GP6 high or as inputs with those external pull-ups. Never hard-tie IO2/IO3 to 3V3 when they are used as QSPI data signals. The future QSPI-PIO driver can sample the contiguous data window GP3-GP6 and must configure the flash's quad-enable setting before quad transfers. Mode changes require protocol reinitialization, not just changing the GPIO mux. PicoForge does not yet implement an external flash driver.

## User I/O Pin List

This table identifies potential future user-expansion GPIOs after assigning UART0 to GP0/GP1, the memory interface to GP2-GP7, and optional SD-card detect to GP8; it does not imply these expansion features are implemented. In SD-SPI mode GP5 remains connected to DAT1 and GP6 to DAT2, while GP7 is CS. In SPI-NOR mode, GP5/GP6 remain reserved for WP/HOLD and GP7 is CS. These pins remain reserved with fixed wiring. Availability of the listed pins and peripheral functions depends on the selected memory mode and other pin/controller assignments. All listed pins can also be used as digital GPIO or routed to PIO.

| Available digital GPIO | Header pin | Hardware peripheral functions |
| --- | --- | --- |
| GP9 | 12 | I2C0 SCL, UART1 RX, PWM B4 |
| GP10 | 14 | I2C1 SDA, PWM A5 |
| GP11 | 15 | I2C1 SCL, PWM B5 |
| GP12 | 16 | I2C0 SDA, PWM A6 |
| GP13 | 17 | I2C0 SCL, PWM B6 |
| GP14 | 19 | I2C1 SDA, PWM A7 |
| GP15 | 20 | I2C1 SCL, PWM B7 |
| GP16 | 21 | I2C0 SDA, PWM A0 |
| GP17 | 22 | I2C0 SCL, PWM B0 |
| GP18 | 24 | I2C1 SDA, PWM A1 |
| GP19 | 25 | I2C1 SCL, PWM B1 |
| GP20 | 26 | I2C0 SDA, UART1 TX, PWM A2 |
| GP21 | 27 | I2C0 SCL, UART1 RX, PWM B2 |
| GP22 | 29 | I2C1 SDA, PWM A3 |
| GP26 | 31 | I2C1 SDA, PWM A5, ADC0 |
| GP27 | 32 | I2C1 SCL, PWM B5, ADC1 |
| GP28 | 34 | I2C0 SDA, PWM A6, ADC2 |

These are alternate mux functions, not simultaneous assignments. UART0 TX/RX are allocated to GP0/GP1. For I2C, use matching SDA/SCL pins from the same controller. Some alternate UART functions differ between RP2040 and RP2350. If DET is not connected, GP8 is available as I2C0 SDA, UART1 TX, or PWM A4.

### Complete SPI Pin Groups

Complete hardware SPI pin groups (CS may be software-controlled):

| Controller option | MISO / RX | MOSI / TX | SCK | CS |
| --- | --- | --- | --- | --- |
| SPI0 option A | GP16 (pin 21) | GP19 (pin 25) | GP18 (pin 24) | GP17 (pin 22) |
| SPI0 option B | GP20 (pin 26) | GP19 (pin 25) | GP22 (pin 29) | GP21 (pin 27) |
| SPI1 | GP28 (pin 34) | GP27 (pin 32) | GP26 (pin 31) | GP15 (pin 20), software GPIO |

SPI0 options A/B are alternatives. The GP16-GP22 SPI0 groups are available for expansion only when the memory device is not using SPI0; an SPI-mode storage profile and an SPI0 expansion device cannot operate independently at the same time, even on different GPIO mappings. SPI1 is the independent expansion controller while storage uses SPI0. Select only one pin mapping per controller and avoid GPIO conflicts with other assignments. GP23/GP24 are used internally for SMPS control and USB VBUS sensing; GP25 drives the onboard LED and GP29 senses VSYS. These pins are not on the user header. See the [official Pico pin functions](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#pin_functions).

## Firmware Resource Mapping

Firmware resources that do not use header GPIOs:

| Feature | RP2040 / RP2350 resource | External pin required | Notes |
| --- | --- | --- | --- |
| System telemetry | SDK MCU identity, system clock, and uptime | No | Exposed through HID; does not reserve GPIOs. |
| Internal temperature | On-chip temperature sensor, ADC input 4 | No | Firmware enables the internal sensor and reads ADC channel 4; this is not GPIO 4. |

When changing `PICO_BOARD`, verify its pin definitions and keep `firmware/src/board/` aligned. Select SDIO-PIO or SPI at build time in `firmware/src/config/config.h`; startup attempts initialization but does not mount a filesystem. Validate card I/O on hardware before enabling storage use.

## Electrical and Power Baseline

- Pico/Pico 2 GPIO is 3.3 V only. Never apply 5 V or RS-232 levels directly.
- Share ground with peripherals; check their voltage and current requirements before using the board's 3V3 rail.
- Check the selected board schematic for pin limits, USB routing, and power behavior. Use level shifting when required.
- For UART, cross TX/RX and share ground.

## Bring-Up Checklist

1. Confirm the board variant and verify header numbering against its pinout.
2. Select an SD-card and flash mode from the pin map; check the chosen GPIOs for conflicts before wiring.
3. Verify 3.3 V compatibility, common ground, and power budget before wiring.
4. Check continuity and rails, then bring up one interface at a time.
5. Keep board support and this document aligned; record hardware validation results.