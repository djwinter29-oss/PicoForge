# PicoForge Hardware Setup Options

Reference hardware setup for standard Pico/Pico 2 boards and external breakouts. The memory and storage pin map below lists the candidate GPIO allocation for each interface mode. Select one mode per connected device. Some options are not implemented in firmware. `TBD` means unassigned.

## Board and Build Selection

| Item | Selection |
| --- | --- |
| Reference board | Raspberry Pi Pico, standard 40-pin header |
| Alternate build target | Raspberry Pi Pico 2 (`PICO_BOARD=pico2`); the firmware build defaults to `pico` |
| MCU | RP2040 on Pico; RP2350 on Pico 2 |
| Reference power | USB through the board's USB connector |

## Debug and Monitoring

Serial and SWD do not overlap the memory pins below. Share ground with the adapter or probe; ground pins are omitted from the tables.

### Serial

| MCU GPIO | Header pin | UART0 |
| --- | --- | --- |
| GP0 | 1 | TX |
| GP1 | 2 | RX |

Connect MCU TX to adapter RX and MCU RX to adapter TX. Board support initializes the monitor UART from the selected topology; the Pico/Pico 2 profiles assign UART0 to GP0/GP1 at 115200 baud, 8-N-1. Logs are output on TX; no command console or RTS/CTS flow control is implemented. Use 3.3 V logic, never RS-232 levels. USB CDC remains a separate interface.

### SWD

| MCU pad | Header pin | SWD |
| --- | --- | --- |
| SWDIO debug pad | N/A | SWDIO |
| SWCLK debug pad | N/A | SWCLK |

Connect probe SWDIO/SWCLK to the matching board pads. Verify their labels on the selected board; these are not numbered pins on the 40-pin header.

## Memory and Storage Connections

Use GP2-GP7 for one memory device and choose one mode column below. Wiring stays fixed between that device's PIO and SPI modes; both SPI modes use SPI0 with CS connected to GP5. Automatic hardware CS is a design goal, not a validated configuration.

| MCU GPIO | Header pin | SDIO-PIO | SD-SPI (SPI0) | QSPI-PIO | SPI-NOR flash (SPI0) |
| --- | --- | --- | --- | --- | --- |
| GP2 | 4 | CLK | CLK / SCK | CLK | CLK / SCK |
| GP3 | 5 | CMD | SI / MOSI | IO0 | IO0 / MOSI |
| GP4 | 6 | DAT0 | SO / MISO | IO1 | IO1 / MISO |
| GP5 | 7 | DAT3 | CS (SPI0 CSn-capable; control method pending validation) | CS | CS (SPI0 CSn-capable; control method pending validation) |
| GP6 | 9 | DAT2 | - | IO2 | WP (hold high) |
| GP7 | 10 | DAT1 | - | IO3 | HOLD (hold high) |
| GP8 | 11 | DET (optional) | DET (optional) | - | - |

- `-` means unused in that mode, not necessarily disconnected. GP8 is optional SD-card detect.
- Use 3.3 V and common ground, never 5 V. Power off before replacing a breakout.
- The board profile explicitly assigns CLK=GP2, CMD=GP3, and logical DAT0-DAT3 to GP4/GP7/GP6/GP5. PIO samples the contiguous GP4-GP7 window; the SDIO transfer code remaps the DAT1/DAT3 bit lanes to match this wiring.
- Future SPI drivers must demonstrate with a logic analyzer that automatic CS remains low across each complete SD/flash transaction, including transfer gaps; a valid CSn pin mux alone does not establish this behavior.
- If automatic CS cannot meet the protocol requirements, hardware SPI0 with GPIO-controlled CS on the same GP5 wire is a proposed fallback requiring no rewiring. This does not satisfy the hardware-CS goal; if hardware CS is mandatory, the design remains unresolved until a suitable transaction-control approach is demonstrated.
- Pin names follow the breakout guides below; wiring, signal integrity, and protocol operation remain unverified on hardware.

### Lab Hardware Examples

The lab uses two types of memory/storage breakout: the Adafruit microSD board and W25Q128 flash board described below. These are reference examples, not an exclusive list of compatible devices. Other SD-card or SPI/QSPI memory devices can use this pin allocation if their supply and logic levels, signal functions, pull-ups, and timing requirements are compatible. Check each device's pinout and protocol; firmware may need device-specific initialization or commands. The active-high DET behavior and WP/HOLD requirements below describe these particular lab boards and must be checked for any replacement. Use in the lab does not establish that all interface modes have been validated.

#### SD-Card Breakout

The selected module is Adafruit's [Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its [pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Keep the breakout's DAT3/CS connected to GP5 in both modes. DAT1/DAT2 remain connected to GP7/GP6; leave those GPIOs as inputs in SD-SPI mode rather than driving them for other functions. The board profile configures its active-high DET signal on GP8 and relies on the breakout's pull-up, without an MCU pull. The breakout and driver both provide SDIO data pull-ups, so check their combined strength. The board profile provides this SDIO configuration, but startup does not initialize or mount the card; physical wiring and card I/O still require validation. Mode changes require protocol reinitialization and may require cycling card power; fixed wiring does not imply hot switching. SD-SPI firmware is not implemented.

#### W25Q128 Flash Breakout

The [Adafruit QSPI DIP Breakout - W25Q128](https://www.adafruit.com/product/5634) is a 3.3 V, 16 MB serial NOR flash supporting QSPI and SPI; see its [pinout guide](https://learn.adafruit.com/adafruit-qspi-dip-breakout-boards/pinouts). Keep CLK, IO0, IO1, CS, IO2, and IO3 connected to GP2, GP3, GP4, GP5, GP6, and GP7 respectively in both modes. Use pull-ups on CS, IO2/WP, and IO3/HOLD to keep them high through reset; in SPI-NOR mode, keep GP6/GP7 high or as inputs with those external pull-ups. Never hard-tie IO2/IO3 to 3V3 when they are used as QSPI data signals. The future QSPI-PIO driver must support the nonconsecutive data mapping (IO0=GP3, IO1=GP4, IO2=GP6, IO3=GP7) and configure the flash's quad-enable setting before quad transfers. Mode changes require protocol reinitialization, not just changing the GPIO mux. PicoForge does not yet implement an external flash driver.

## User I/O Pin List

This table identifies potential future user-expansion GPIOs after assigning UART0 to GP0/GP1, the memory interface to GP2-GP7, and optional SD-card detect to GP8; it does not imply these expansion features are implemented. SD-SPI leaves GP6/GP7 unused by the protocol but still connected to the card, so they remain reserved with fixed wiring. In SPI-NOR mode, GP6/GP7 remain reserved for the flash's WP/HOLD signals and must stay high. Availability of the listed pins and peripheral functions depends on the selected memory mode and other pin/controller assignments. All listed pins can also be used as digital GPIO or routed to PIO.

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

When changing `PICO_BOARD`, verify its pin definitions and keep `firmware/src/board/` aligned. SDIO is available through the board configuration API but is not initialized or mounted automatically; validate the wiring on hardware before enabling storage use.

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