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
| Pico SDK version | See `tools/firmware/setup-sdk-env.sh` |
| Power source | TBD |

## Signal Assignment

Record MCU GPIO numbers and physical connector pin numbers separately. Directions are from the MCU's perspective. Confirm every assignment against the selected board schematic and the MCU datasheet.

| Interface | Signal | MCU GPIO | Board connector / pin | MCU direction | Status / notes |
| --- | --- | --- | --- | --- | --- |
| Debug UART | TX | TBD | TBD | Output | Connect to peer RX |
| Debug UART | RX | TBD | TBD | Input | Connect to peer TX |
| Debug UART | GND | N/A | TBD | Reference | Share ground with peer |
| SDIO | CLK | TBD | TBD | Output | Confirm controller and board routing |
| SDIO | CMD | TBD | TBD | Bidirectional | Confirm pull-ups and voltage requirements |
| SDIO | DAT0 | TBD | TBD | Bidirectional | Required data line |
| SDIO | DAT1 | TBD | TBD | Bidirectional | Required for 4-bit mode |
| SDIO | DAT2 | TBD | TBD | Bidirectional | Required for 4-bit mode |
| SDIO | DAT3 | TBD | TBD | Bidirectional | Required for 4-bit mode |
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
- For SDIO, confirm the selected bus width, required pull-ups, card-detect behavior, and signal routing against the card interface and board design.

## Bring-Up Checklist

1. Identify the exact board and revision; verify connector numbering against its schematic.
2. Fill every used signal's GPIO, connector pin, direction, active level, and pull configuration in the table.
3. Check that no pin is assigned to conflicting board functions or reserved for debug, flash, or another peripheral.
4. Verify voltage compatibility, ground reference, and power budget before attaching peripherals.
5. Check continuity and power rails before applying power; bring up one interface at a time.
6. Keep this document aligned with the board-support configuration and record the board revision used for validation.