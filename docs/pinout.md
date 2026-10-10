# Hardware Pinout and Wiring Baseline

This document is the starting point for connecting PicoForge to standard Raspberry Pi Pico or Pico 2 boards and peripheral breakouts. It records a candidate SD-card wiring and the remaining decisions required before board bring-up.

> **Status: Draft.** The target is a standard Pico or Pico 2 board. The SD-card mapping below is a candidate and is not enabled in firmware; confirm breakout compatibility and wiring before connecting hardware. `TBD` means unassigned.

## Target Identification

| Item | Selection |
| --- | --- |
| Board / module | Raspberry Pi Pico or Pico 2, standard 40-pin header |
| MCU and package | RP2040 (Pico) or RP2350 (Pico 2) |
| Power source | USB |

## Signal Assignment

Record MCU GPIO numbers and physical connector pin numbers separately. GPIO assignments are listed in ascending GPIO order. Directions are from the MCU's perspective. Confirm every assignment against the selected board schematic and the MCU datasheet.

| Interface | Signal | MCU GPIO | Board connector / pin | MCU direction | Status / notes |
| --- | --- | --- | --- | --- | --- |
| UART0 | TX | 0 | Physical pin 1 | Output | Candidate UART connection; connect to peer RX |
| UART0 | RX | 1 | Physical pin 2 | Input | Candidate UART connection; connect to peer TX |
| QSPI | CS | 10 | Physical pin 14 | Output | Candidate mapping to flash breakout `CS` |
| QSPI | CLK | 11 | Physical pin 15 | Output | Candidate mapping to flash breakout `CLK` |
| QSPI | IO0 | 12 | Physical pin 16 | Bidirectional | Candidate mapping to flash breakout `IO0` |
| QSPI | IO1 | 13 | Physical pin 17 | Bidirectional | Candidate mapping to flash breakout `IO1` |
| QSPI | IO2 | 14 | Physical pin 19 | Bidirectional | Candidate mapping to flash breakout `IO2` |
| QSPI | IO3 | 15 | Physical pin 20 | Bidirectional | Candidate mapping to flash breakout `IO3` |
| SDIO | CLK | 16 | Physical pin 21 | Output | Candidate mapping to breakout `CLK`; derived as `DAT0 - 2` |
| SD card detect | DET | 17 | Physical pin 22 | Input | Optional candidate near the SDIO signals; high when card is inserted, low when absent; no extra pull configured |
| SDIO | DAT0 | 18 | Physical pin 24 | Bidirectional | Candidate mapping to breakout `D0`; DAT0-DAT3 must be consecutive GPIOs |
| SDIO | DAT1 | 19 | Physical pin 25 | Bidirectional | Candidate mapping to breakout `D1` |
| SDIO | DAT2 | 20 | Physical pin 26 | Bidirectional | Candidate mapping to breakout `DAT2` |
| SDIO | DAT3 | 21 | Physical pin 27 | Bidirectional | Candidate mapping to breakout `D3` |
| SDIO | CMD | 22 | Physical pin 29 | Bidirectional | Candidate mapping to breakout `CMD` |
| Board LED | Onboard LED | 25 on Pico / Pico 2 | Onboard LED; no external connector assignment | Output | Current SDK board definitions set `PICO_DEFAULT_LED_PIN` to GPIO 25; board support configures active-high. Verify the selected board definition and schematic before relying on this mapping. |
| UART0 | GND | N/A | Physical pin 3 | Reference | Common ground |
| Power | Flash breakout `3V` | N/A | Physical pin 36 | Power | 3.3 V only; share the board 3V3 rail if its current budget permits |
| Power | SD breakout `3V` | N/A | Physical pin 36 | Power | Only if the board's 3V3 rail can supply the breakout; verify its requirements |
| Ground | Flash breakout `G` | N/A | Physical pin 38 | Reference | Common ground |
| Ground | SD breakout `GND` | N/A | Physical pin 38 | Reference | Common ground |
| USB | D+ / D- | Board connector | TBD | Bidirectional | Use the board's supported USB connector and routing |
| SWD | SWDIO | Board debug pads | TBD | Bidirectional | Confirm board-specific access |
| SWD | SWCLK | Board debug pads | TBD | Input to target | Confirm board-specific access |
| SWD | GND | N/A | TBD | Reference | Required for the debug probe |

The SDIO and QSPI assignments are proposed wiring for the two breakouts below, not enabled board configurations. The SDIO driver derives CLK from DAT0 and requires PIO0 or PIO1. The QSPI mapping uses four bidirectional data GPIOs plus separate CLK and CS GPIOs. Add rows for any additional buses, control signals, or external peripherals before connecting them. Note active polarity, pull resistors, and any alternate-function conflicts.

The earlier lower-numbered SDIO example was only one valid arrangement; SDIO does not require low GPIO numbers. This mapping uses the upper header GPIO range for both storage interfaces and reserves physical pins 1 and 2 (GP0 and GP1) for UART0.

The standard Pico and Pico 2 expose 26 GPIOs on their headers. UART0 uses 2, SDIO uses 6, and QSPI uses 6, leaving 12 user GPIOs without card detect: GP2-GP9, GP17, and GP26-GP28. If optional card detect uses GP17, 11 remain: GP2-GP9 and GP26-GP28. The onboard LED uses GP25, which is not a header pin.

GP23 and GP24 are not listed as user pins because the standard Pico/Pico 2 board uses them internally: GP23 controls the onboard SMPS power-save function, and GP24 senses USB VBUS. GP25 drives the onboard LED, and GP29 senses VSYS. These four signals are not exposed on the 40-pin header, so the 26 header GPIOs are GP0-GP22 and GP26-GP28. See the [official Raspberry Pi Pico board layout and pin functions](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#pin_functions).

## Firmware Resource Mapping

Not every firmware-facing value corresponds to a connector pin:

| Feature | RP2040 / RP2350 resource | External pin required | Notes |
| --- | --- | --- | --- |
| System telemetry | SDK MCU identity, system clock, and uptime | No | Exposed through HID; does not reserve GPIOs. |
| Internal temperature | On-chip temperature sensor, ADC input 4 | No | Firmware enables the internal sensor and reads ADC channel 4; this is not GPIO 4. |
| Board LED | Board-defined `PICO_DEFAULT_LED_PIN` | No external LED wiring for the onboard LED | The current `pico` and `pico2` SDK board definitions use GPIO 25. Other board definitions may use a different pin or omit the LED. |

These defaults describe the current SDK board targets, not an approved PicoForge PCB pinout. When changing `PICO_BOARD`, confirm its SDK definition and schematic, then keep `firmware/src/board/` and this document aligned. The SDIO interface remains disabled in board support until its GPIO and connector assignments are approved.

## SD-Card Breakout

The SD breakout is the [Adafruit Micro SD SPI or SDIO Card Breakout Board](https://www.adafruit.com/product/4682), marked P4682D. It is 3.3 V power and logic only, supports SPI or SDIO, and provides the SDIO pads `CLK`, `CMD`, `D0`, `D1`, `DAT2`, and `D3`. See Adafruit's [pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). The table maps those six pads to the driver's required 4-bit SDIO arrangement. The optional `DET` output is low when no card is present and pulled high when a card is inserted; if connected as shown, configure card detect as active-high and do not enable an additional GPIO pull.

The driver enables pull-ups on CMD and DAT0-DAT3; the Adafruit breakout also provides pull-ups on its SDIO logic pins, so verify the combined pull-up strength. The proposed GPIO-to-header mapping does not itself enable the driver. `firmware/src/board/board.c` currently sets `sdio_enabled` to false and leaves SD pins unassigned. Update board support only after confirming the wiring and power budget and validating the board topology. SPI-only operation is not wired in this candidate map.

## External QSPI Flash

The flash breakout is the [Adafruit QSPI DIP Breakout Board - W25Q128](https://www.adafruit.com/product/5634), using the W25Q128JVSSIQ 128-Mbit (16-Mbyte) 3.3 V flash. See Adafruit's [QSPI breakout pinout guide](https://learn.adafruit.com/adafruit-qspi-dip-breakout-boards/pinouts). The candidate map connects `CS`, `CLK`, and all four bidirectional data lines (`IO0`-`IO3`) to separate Pico/Pico 2 GPIOs for true QSPI operation. This flash variant has quad mode enabled by default. The RP2040/RP2350 onboard boot-flash QSPI signals are not available as a general-purpose bus on the 40-pin header; this proposal uses GPIO/PIO instead.

PicoForge currently has no external W25Q128 QSPI driver, so this is a wiring proposal only. The QSPI pins do not conflict with the candidate SDIO pins above, allowing both breakouts to be connected at once. Firmware still needs a QSPI driver and PIO resource allocation before the flash can be used alongside SDIO. Confirm the combined 3V3 current budget before powering both breakouts from the Pico/Pico 2 rail.

## Electrical and Power Baseline

- Treat RP2040 and RP2350 GPIO as 3.3 V logic. Never connect 5 V logic or RS-232 voltage levels directly to a GPIO.
- Check the selected board's schematic and datasheet for GPIO limits, pin availability, USB wiring, and power-path behavior.
- Share a ground reference between the board and each non-isolated peripheral.
- Choose and document one power source and its current budget before connecting external hardware. Do not assume a board can power attached peripherals from its 3.3 V rail.
- Use level shifting or a suitable transceiver when a peripheral's signal levels are not compatible with the board.
- For UART, cross TX and RX. Connect RTS/CTS only when both endpoints support the selected flow-control mode.
- For the current 4-bit PIO SD-card backend, use PIO0 or PIO1 and GPIOs below 32; place DAT0-DAT3 on four consecutive GPIOs, with DAT0 at least GPIO2 and CLK at DAT0-2. The candidate mapping in this document uses DAT0=GPIO18 and CLK=GPIO16. Reserve the selected PIO instruction memory for the backend. Confirm pull-ups, optional card-detect wiring, voltage, and signal routing against the board design.
- The W25Q128 candidate QSPI wiring uses GPIO10-15; this external-flash path is not implemented in firmware and requires PIO/resource planning alongside SDIO.

## Bring-Up Checklist

1. Identify whether the board is a Pico or Pico 2 and verify its revision and connector numbering against its schematic.
2. Select the peripheral breakout and verify that its interface and voltage requirements match the proposed wiring.
3. Fill every used signal's GPIO, connector pin, direction, active level, and pull configuration in the table.
4. Check that no pin is assigned to conflicting board functions or reserved for debug, flash, or another peripheral.
5. Verify voltage compatibility, ground reference, and power budget before attaching peripherals.
6. Check continuity and power rails before applying power; bring up one interface at a time.
7. Keep this document aligned with the board-support configuration and record the board revision used for validation.