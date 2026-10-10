# Hardware Pinout and Wiring Baseline

This document is the starting point for connecting PicoForge hardware. It records the board, connector, power, and signal assignments that must be agreed before wiring or board bring-up.

> **Status: Draft.** No PicoForge board or GPIO mapping has been approved. `TBD` means unassigned; do not use this document alone to wire a board.

## Target Identification

| Item | Selection |
| --- | --- |
| Board / module | TBD |
| Board revision | TBD |
| MCU and package | RP2040 or RP2350 variant TBD |
| Schematic revision | TBD |
| Pico SDK version | 2.3.0, pinned by `firmware/third_party/pico-sdk` |
| Power source | TBD |

## Signal Assignment

Record MCU GPIO numbers and physical connector pin numbers separately. Directions are from the MCU's perspective. Confirm every assignment against the selected board schematic and the MCU datasheet.

| Interface | Signal | MCU GPIO | Board connector / pin | MCU direction | Status / notes |
| --- | --- | --- | --- | --- | --- |
| Debug UART | TX | TBD | TBD | Output | Connect to peer RX |
| Debug UART | RX | TBD | TBD | Input | Connect to peer TX |
| Debug UART | GND | N/A | TBD | Reference | Share ground with peer |
| SDIO | CLK | TBD | TBD | Output | Native PIO host derives CLK as `(DAT0 - 2) mod 32` |
| SDIO | CMD | TBD | TBD | Bidirectional | Must not overlap CLK or DAT0-DAT3; confirm pull-ups and voltage |
| SDIO | DAT0 | TBD | TBD | Bidirectional | Native PIO host base; DAT0-DAT3 must be consecutive GPIOs |
| SDIO | DAT1 | TBD | TBD | Bidirectional | Derived as DAT0 + 1 |
| SDIO | DAT2 | TBD | TBD | Bidirectional | Derived as DAT0 + 2 |
| SDIO | DAT3 | TBD | TBD | Bidirectional | Derived as DAT0 + 3 |
| SD card | Detect | TBD / unused | TBD / N/A | Input | Optional; document active level and pull-up/down |
| USB | D+ / D- | Board connector | TBD | Bidirectional | Use the board's supported USB connector and routing |
| SWD | SWDIO | Board debug pads | TBD | Bidirectional | Confirm board-specific access |
| SWD | SWCLK | Board debug pads | TBD | Input to target | Confirm board-specific access |
| SWD | GND | N/A | TBD | Reference | Required for the debug probe |

Add rows for any additional buses, control signals, or external peripherals before connecting them. Note active polarity, pull resistors, and any alternate-function conflicts.

## Electrical and Power Baseline

- Treat RP2040 and RP2350 GPIO as 3.3 V logic. Never connect 5 V logic or RS-232 voltage levels directly to a GPIO.
- Check the selected board's schematic and datasheet for GPIO limits, pin availability, USB wiring, and power-path behavior.
- Share a ground reference between the board and each non-isolated peripheral.
- Choose and document one power source and its current budget before connecting external hardware. Do not assume a board can power attached peripherals from its 3.3 V rail.
- Use level shifting or a suitable transceiver when a peripheral's signal levels are not compatible with the board.
- For UART, cross TX and RX. Connect RTS/CTS only when both endpoints support the selected flow-control mode.
- For the current 4-bit PIO SD-card backend, use PIO0 or PIO1 and GPIOs below 32; place DAT0-DAT3 on four consecutive GPIOs, with DAT0 at least GPIO2 and CLK at DAT0-2. Reserve the selected PIO instruction memory for the backend. Confirm pull-ups, optional card-detect wiring, voltage, and signal routing against the board design.

## Bring-Up Checklist

1. Identify the exact board and revision; verify connector numbering against its schematic.
2. Fill every used signal's GPIO, connector pin, direction, active level, and pull configuration in the table.
3. Check that no pin is assigned to conflicting board functions or reserved for debug, flash, or another peripheral.
4. Verify voltage compatibility, ground reference, and power budget before attaching peripherals.
5. Check continuity and power rails before applying power; bring up one interface at a time.
6. Keep this document aligned with the board-support configuration and record the board revision used for validation.