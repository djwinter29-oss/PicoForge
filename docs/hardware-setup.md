# PicoForge Hardware Setup Options

Reference hardware setup for standard Pico/Pico 2 boards and external breakouts. See the
[storage transport guide](../firmware/src/drivers/storage/README.md) for memory/storage modes, wiring, electrical
requirements, and qualification records. Select one mode per connected device. Some options are not implemented in
firmware. `TBD` means unassigned.

## Board and Build Selection

| Item | Selection |
| --- | --- |
| Reference board | Raspberry Pi Pico, standard 40-pin header |
| Alternate build target | Raspberry Pi Pico 2 (`PICO_BOARD=pico2`); the firmware build defaults to `pico` |
| MCU | RP2040 on Pico; RP2350 on Pico 2 |
| Reference power | USB through the board's USB connector |
| Optional overclock | `tools/firmware/build.sh --board pico2 --sys-clk-mhz 200` (or `PF_SYS_CLK_MHZ=200` with `load.sh`); out of spec, range 100-250 MHz, default `0` keeps the SDK clock. The SDK applies it at boot and raises the regulator to 1.15 V above 150 MHz and 1.30 V above 200 MHz. The SDIO board profile requests 25 MHz; at the stock 150 MHz system clock its integer slow-mode divider produces exactly 25 MHz. |

## Debug and Monitoring

Serial and SWD do not overlap the [storage pins](../firmware/src/drivers/storage/README.md#memory-and-storage-connections).
Share ground with the adapter or probe; ground pins are omitted from the tables.

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

The [storage transport guide](../firmware/src/drivers/storage/README.md) owns:

- [GP2-GP8 pin mapping and electrical requirements](../firmware/src/drivers/storage/README.md#memory-and-storage-connections).
- [SD-card breakout wiring and historical qualification](../firmware/src/drivers/storage/README.md#sd-card-breakout).
- [SDIO-PIO and SD-SPI transport architecture and lifecycle](../firmware/src/drivers/storage/README.md#physical-storage-bus).

## User I/O Pin List

This table identifies potential future user-expansion GPIOs after assigning UART0 to GP0/GP1, the SD-card interface
to GP2-GP7, and optional SD-card detect to GP8; it does not imply these expansion features are implemented. In SD-SPI
mode GP5 and GP6 are unused by the card, while GP7 is CS. Availability of listed pins and peripheral functions
depends on selected transport and other pin/controller assignments. All listed pins can also be used as digital GPIO
or routed to PIO.

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

When changing `PICO_BOARD`, verify its pin definitions and keep `firmware/src/board/` aligned. Select SDIO-PIO or SPI at build time in `firmware/src/config/config.h`; detected cards are mounted by the storage worker as described in [the mounting policy](cdc-cli.md). Validate card I/O on hardware before enabling storage use.

## Electrical and Power Baseline

- Pico/Pico 2 GPIO is 3.3 V only. Never apply 5 V or RS-232 levels directly.
- Share ground with peripherals; check their voltage and current requirements before using the board's 3V3 rail.
- Check the selected board schematic for pin limits, USB routing, and power behavior. Use level shifting when required.
- For UART, cross TX/RX and share ground.

## Bring-Up Checklist

1. Confirm the board variant and verify header numbering against its pinout.
2. Select SDIO-PIO or SD-SPI from the pin map; check the chosen GPIOs for conflicts before wiring.
3. Verify 3.3 V compatibility, common ground, and power budget before wiring.
4. Check continuity and rails, then bring up one interface at a time.
5. Keep board support and this document aligned; record hardware validation results.