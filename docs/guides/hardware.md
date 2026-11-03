# Hardware Setup

This guide owns board wiring, power, debug connections, and the SD-card breakout setup.
Supported builds target Pico, Pico W, Pico 2, and Pico 2 W. Choose one SD transport at build time. Expansion GPIO examples below
describe available hardware functions, not implemented application features.

## Board and Build Selection

Board GPIO assignments use `uint8_t`; `UINT8_MAX` (`255`) means no pin. An absent LED is disabled, an absent UART
TX/RX direction is passed to the SDK as `-1`, and absent card detect assumes a card is present without reading GPIO.
All SPI and SDIO bus signals are required: an unassigned bus pin is rejected during initialization. SDK GPIO
arguments and GPIO bit masks retain their SDK-defined widths.

[config.h](../../firmware/src/config/config.h) keeps the SDK-derived LED selection and UART/LED fallback macros;
[board.c](../../firmware/src/config/board.c) defines the corresponding assignment objects.

[board_spi.c](../../firmware/src/config/board_spi.c) and [board_sdio.c](../../firmware/src/config/board_sdio.c) define
the complete SPI and SDIO-PIO assignments. Each groups its bus pins, byte-sized controller index, and card-detect
GPIO/polarity. CMake compiles only the selected assignment source alongside its matching transport binding;
there is no separate selector macro, shared SD-board struct, or union. Reusable engines receive bus signals only;
the SD binding manages card detection.

| Item | Selection |
| --- | --- |
| Reference board | Raspberry Pi Pico, standard 40-pin header |
| Other build targets | Pico W (`pico_w`), Pico 2 (`pico2`), and Pico 2 W (`pico2_w`); defaults to `pico` |
| MCU | RP2040 on Pico/Pico W; RP2350 on Pico 2/Pico 2 W |
| Reference power | USB through the board's USB connector |
| Optional clock profile | Saved profiles apply at reboot; builds use the SDK stock clock. Runtime clock changes do not raise core voltage. Out-of-spec profiles need board qualification; see [clock safety](../reference/settings.md#system-clock). |

## Debug and Monitoring

Serial and SWD do not overlap the
[storage pins](hardware.md#memory-and-storage-connections).
Share ground with the adapter or probe; ground pins are omitted from the tables.

### Serial

| MCU GPIO | Header pin | UART0 |
| --- | --- | --- |
| GP0 | 1 | TX |
| GP1 | 2 | RX |

Connect MCU TX to adapter RX and MCU RX to adapter TX, and connect GND. Board support uses the selected Pico SDK board's
default UART mapping; the supported Pico-family boards assign UART0 to GP0/GP1 at 115200 baud, 8-N-1. This keeps the monitor
wiring aligned with the [PicoUart board setup](https://github.com/djwinter29-oss/PicoUart/blob/main/docs/hardware-setup.md);
PicoForge uses the UART only for independent debug logging. UART logging has been verified on a Pico through the
Raspberry Pi Debug Probe. The firmware emits boot messages and a heartbeat each second, including uptime and system
clock. On Linux the probe's UART bridge appears as a serial device (for example `/dev/ttyACM0`; device numbering can
vary), while PicoForge USB CDC is a separate port (for example `/dev/ttyACM1`). Open the probe UART at 115200 baud,
for example with `screen /dev/ttyACM0 115200`. Reset the Pico after opening the terminal to see boot messages. No
command console or RTS/CTS flow control is implemented; text sent to UART RX is not processed. Use 3.3 V logic, never
RS-232 levels. USB CDC remains a separate interface.

### SWD

| MCU pad | Header pin | SWD |
| --- | --- | --- |
| SWDIO debug pad | N/A | SWDIO |
| SWCLK debug pad | N/A | SWCLK |

Connect probe SWDIO/SWCLK to the matching board pads. Verify their labels on the selected board; these are not numbered pins on the 40-pin header.

## Memory and Storage Connections

Use GP2-GP7 for one SD card and choose one mode below. GP7 is the CS pin in SD-SPI and is controlled by GPIO, not
automatic hardware CS. SDIO has no CS signal.

| MCU GPIO | Header pin | SDIO-PIO | SD-SPI (SPI0) |
| --- | --- | --- | --- |
| GP2 | 4 | CLK | CLK / SCK |
| GP3 | 5 | CMD | SI / MOSI |
| GP4 | 6 | DAT0 | SO / MISO |
| GP5 | 7 | DAT1 | - |
| GP6 | 9 | DAT2 | - |
| GP7 | 10 | DAT3 | CS (GPIO-controlled) |
| GP8 | 11 | DET (optional) | DET (optional) |

- `-` means unused in that mode, not necessarily disconnected. GP8 is optional SD-card detect.
- Use 3.3 V and common ground, never 5 V. Power off before replacing a breakout.
- SDIO-PIO requires logical DAT0-DAT3 on four ascending consecutive GPIOs, in that order; lane swaps are unsupported.
  The board profile assigns CLK=GP2, CMD=GP3, and DAT0-DAT3 to GP4/GP5/GP6/GP7. PIO samples the contiguous GP4-GP7
  window directly.
- The SD-SPI driver holds GPIO-controlled CS low across each complete card transaction, including data and
  busy-response phases. Verify CS timing, clock, and data with a logic analyzer during board validation.
- SDIO has no CS signal. In SD-SPI mode, the driver controls the breakout's DAT3/CS line on GP7 as ordinary GPIO;
  no automatic hardware CS is used.
- Pin names follow the breakout guide below. Historical operations are preserved in
  [bring-up records](../validation/records/sdcard-bringup-observations.md); they do not qualify current firmware.

## Lab Hardware Examples

The lab uses the Adafruit microSD breakout described below as a reference example, not an exclusive list of compatible
devices. Check each card and breakout's pinout, supply, logic levels, pull-ups and timing requirements. Use in the lab
does not establish that all cards or interface modes have been validated.

### SD-Card Breakout

The selected module is Adafruit's
[Micro SD SPI or SDIO Card Breakout Board (P4682D)](https://www.adafruit.com/product/4682); see its
[pinout guide](https://learn.adafruit.com/adafruit-microsd-spi-sdio/pinouts). It is 3.3 V only. Wire
DAT0/DAT1/DAT2/DAT3-CS to GP4/GP5/GP6/GP7 respectively in both modes. DAT1 and DAT2 are unused inputs in SD-SPI mode;
GP7 drives CS in that mode. Board support can construct either transport configuration and sets its active-high DET
signal on GP8 using the breakout's pull-up, without an MCU pull. Both drivers explicitly disable inherited MCU pulls
when the external DET pull-up is selected. The breakout and driver both provide SDIO data pull-ups, so check their
combined strength. The storage worker initializes and mounts detected cards according to
[the mounting policy](../reference/architecture.md#storage-ownership-lifecycle). SPI uses CMD18/CMD25 for multi-sector requests and CMD17/CMD24
for single sectors. Card capability negotiation and the saved storage speed target are defined by the
[SPI transfer-clock policy](../reference/sdcard/driver.md#spi-transfer-clock); this is not a guaranteed 50 MHz bus.
Mode changes require protocol reinitialization. When switching from SPI to native SDIO, fully remove card power:
an MCU reset alone can leave the card latched in SPI mode. Fixed wiring does not imply hot switching.

## User I/O Pin List

This table identifies hardware mux options for downstream projects after assigning UART0 to GP0/GP1, the SD-card interface
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

SPI0 options A/B are alternatives. The GP16-GP22 SPI0 groups are available for expansion only when the SD card is
not using SPI0; an SD-SPI profile and an SPI0 expansion device cannot operate independently on different GPIO
mappings. SPI1 is the independent expansion controller while SD uses SPI0. Select only one pin mapping per controller
and avoid GPIO conflicts. On the reference boards, GP23/GP24 serve internal power/USB functions, GP25 drives the LED,
and GP29 senses VSYS; these pins are not on the user header. Verify the selected board's
[official pin functions](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#pin_functions).

## Firmware Resource Mapping

Firmware resources that do not use header GPIOs:

| Feature | RP2040 / RP2350 resource | External pin required | Notes |
| --- | --- | --- | --- |
| System telemetry | SDK MCU identity, system clock, and uptime | No | Exposed through HID; does not reserve GPIOs. |
| Internal temperature | On-chip temperature sensor, ADC input 4 | No | Firmware enables the internal sensor and reads ADC channel 4; this is not GPIO 4. |

When changing `PICO_BOARD`, verify its pin definitions and keep `firmware/src/config/board.c` aligned. Select SDIO-PIO or
SD-SPI with `tools/firmware/build.sh --sdcard-transport SDIO_PIO` or `--sdcard-transport SPI`; CMake links
the matching protocol, physical engine and board binding. Detected cards are mounted by the worker as described in
[the storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle). Validate card I/O on hardware before enabling storage use.

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