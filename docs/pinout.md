# Hardware Pinout and Wiring Baseline

Pinout and breakout wiring for standard Raspberry Pi Pico/Pico 2 boards. The SD interface is not enabled in firmware; verify wiring and power before connecting hardware. `TBD` means unassigned.

## Target Identification

| Item | Selection |
| --- | --- |
| Board / module | Raspberry Pi Pico or Pico 2, standard 40-pin header |
| MCU and package | RP2040 (Pico) or RP2350 (Pico 2) |
| Power source | USB |

## SD-Card Breakout

The selected module is Adafruit's [Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its [pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Use the Signal Assignment below for either SDIO-PIO or hardware SPI0 without rewiring; SPI mode uses GPIO-controlled CS, and DET on GP14 is active-high. The breakout and driver both provide SDIO pull-ups, so check their combined strength. SDIO remains disabled in board support, and SPI-mode firmware is not implemented.

## W25Q128 Flash Breakout

The [Adafruit QSPI DIP Breakout - W25Q128](https://www.adafruit.com/product/5634) is a 3.3 V, 16 MB flash supporting QSPI and SPI; see its [pinout guide](https://learn.adafruit.com/adafruit-qspi-dip-breakout-boards/pinouts). Use the shared pin map above. QSPI-PIO requires support for its noncontiguous data pins. In SPI1 mode, keep IO2/WP and IO3/HOLD high through reset using pull-ups, or isolate them with jumpers and tie them to 3V3. PicoForge does not yet implement either flash driver.

## Signal Assignment

GPIO names and header numbers are separate; rows are ordered by GPIO. Directions are from the MCU's perspective.

| Interface | Signal | MCU GPIO | Header pin | MCU direction | Status / notes |
| --- | --- | --- | --- | --- | --- |
| UART0 | TX | GP0 | 1 | Output | Candidate UART connection; connect to peer RX |
| UART0 | RX | GP1 | 2 | Input | Candidate UART connection; connect to peer TX |
| SDIO-PIO / SPI0 | CLK / SCK | GP2 | 4 | Output | SDIO `CLK`; SPI0 `SCK` in SPI mode |
| SDIO-PIO / SPI0 | CMD / MOSI | GP3 | 5 | Bidirectional | Breakout `CMD`; SPI0 `MOSI` in SPI mode |
| SDIO-PIO / SPI0 | DAT0 / MISO | GP4 | 6 | Bidirectional | Breakout `D0`; SPI0 `MISO` in SPI mode |
| SDIO only | DAT1 (SPI unused) | GP5 | 7 | Bidirectional | Breakout `D1`; not connected to an SPI-mode signal |
| SDIO only | DAT2 (SPI unused) | GP6 | 9 | Bidirectional | Breakout `DAT2`; not connected to an SPI-mode signal |
| SDIO-PIO / SPI0 | DAT3 / CS | GP7 | 10 | Bidirectional | Breakout `D3`; SPI-mode CS is controlled as a GPIO |
| QSPI-PIO / SPI1 | IO1 / MISO | GP8 | 11 | Bidirectional | Flash breakout `IO1`; SPI1 `MISO` in SPI mode |
| QSPI-PIO / SPI1 | CS / CSn | GP9 | 12 | Output | Flash breakout `CS`; SPI1 chip select in SPI mode |
| QSPI-PIO / SPI1 | CLK / SCK | GP10 | 14 | Output | Flash breakout `CLK`; SPI1 `SCK` in SPI mode |
| QSPI-PIO / SPI1 | IO0 / MOSI | GP11 | 15 | Bidirectional | Flash breakout `IO0`; SPI1 `MOSI` in SPI mode |
| QSPI-PIO / SPI1 | IO2 / WP | GP12 | 16 | Bidirectional | Flash `IO2`; hold high in SPI mode |
| QSPI-PIO / SPI1 | IO3 / HOLD | GP13 | 17 | Bidirectional | Flash `IO3`; hold high in SPI mode |
| SD card detect | DET | GP14 | 19 | Input | Optional; high when card is inserted, low when absent; no extra pull configured |
| Board LED | Onboard LED | GP25 | N/A (onboard LED) | Output | SDK default for Pico/Pico 2; active-high |
| USB | D+ / D- | Board connector | N/A (USB connector) | Bidirectional | Use the board's supported USB connector and routing |
| SWD | SWDIO | Board debug pads | N/A (debug pads) | Bidirectional | Confirm board-specific access |
| SWD | SWCLK | Board debug pads | N/A (debug pads) | Input to target | Confirm board-specific access |
| SWD | GND | N/A | N/A (debug pads) | Reference | Required for the debug probe |

Shared wiring supports SDIO-PIO or SPI0 for the card, and QSPI-PIO or SPI1 for flash. Neither external-storage driver is enabled.

With UART0, both breakouts in PIO mode, and DET connected, 11 of 26 header GPIOs remain free. SPI modes free GP5/GP6 (13 free); jumpers that isolate flash IO2/IO3 free GP12/GP13 too (15 free). GP25 drives the onboard LED; it is not on the header.

## User I/O Pin List

This table lists only digital GPIO pins left for application use after the proposed PicoForge allocations: UART0, QSPI-PIO, SDIO-PIO, and SD card detect on GP14. It shows the available hardware-controller mux functions for each pin; all listed pins can also be used as digital GPIO or routed to PIO.

| Available digital GPIO | Header pin | Hardware peripheral functions |
| --- | --- | --- |
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

These are alternate mux functions, not simultaneous assignments. UART0 TX/RX are allocated to GP0/GP1. For I2C, use matching SDA/SCL pins from the same controller. Some alternate UART functions differ between RP2040 and RP2350.

### Complete SPI Pin Groups

Complete hardware SPI groups (CS may be software-controlled):

| Controller option | MISO / RX | MOSI / TX | SCK | CS |
| --- | --- | --- | --- | --- |
| SPI0 option A | GP16 (pin 21) | GP19 (pin 25) | GP18 (pin 24) | GP17 (pin 22) |
| SPI0 option B | GP20 (pin 26) | GP19 (pin 25) | GP22 (pin 29) | GP21 (pin 27) |
| SPI1 | GP28 (pin 34) | GP27 (pin 32) | GP26 (pin 31) | GP15 (pin 20), software GPIO |

SPI0 options A/B are alternatives. These groups cannot be used while that controller is mapped to a breakout on a different pin group. GP23/GP24 are used internally for SMPS control and USB VBUS sensing; GP25 drives the onboard LED and GP29 senses VSYS. These pins are not on the user header. See the [official Pico pin functions](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#pin_functions).

## Firmware Resource Mapping

Firmware resources that do not use header GPIOs:

| Feature | RP2040 / RP2350 resource | External pin required | Notes |
| --- | --- | --- | --- |
| System telemetry | SDK MCU identity, system clock, and uptime | No | Exposed through HID; does not reserve GPIOs. |
| Internal temperature | On-chip temperature sensor, ADC input 4 | No | Firmware enables the internal sensor and reads ADC channel 4; this is not GPIO 4. |

When changing `PICO_BOARD`, verify its pin definitions and keep `firmware/src/board/` aligned. SDIO remains disabled in board support until wiring is approved.

## Electrical and Power Baseline

- Pico/Pico 2 GPIO is 3.3 V only. Never apply 5 V or RS-232 levels directly.
- Share ground with peripherals; check their voltage and current requirements before using the board's 3V3 rail.
- Check the selected board schematic for pin limits, USB routing, and power behavior. Use level shifting when required.
- For UART, cross TX/RX and share ground.

## Bring-Up Checklist

1. Confirm the board variant and verify header numbering against its pinout.
2. Choose each breakout's mode and follow the matching pin assignments; check for conflicts.
3. Verify 3.3 V compatibility, common ground, and power budget before wiring.
4. Check continuity and rails, then bring up one interface at a time.
5. Keep board support and this document aligned; record hardware validation results.