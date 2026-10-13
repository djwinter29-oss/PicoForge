# PicoForge Architecture

> **Status:** The firmware builds for RP2040 and RP2350 and runs single-core FreeRTOS tasks. UART logging has been verified through a Raspberry Pi Debug Probe. Basic system, internal-temperature, board-LED, SDIO-PIO and SPI SD-card transports, FatFs integration, and conditional USB MSC exist. `firmware/src/config/config.h` statically selects the transport. MSC is disabled at boot; startup initializes the card for telemetry but does not mount a filesystem. MSC ownership is exclusive and requires host safe-eject before firmware regains the card. See [the hardware validation record](hardware-setup.md#sd-card-breakout) for verified SDIO and SPI filesystem CLI read/write operations; complete MSC workflows and power-loss recovery remain unverified.

## Current Firmware

`firmware/CMakeLists.txt` imports the Pico SDK selected by `PICO_SDK_PATH`, initializes the SDK, imports the platform-specific FreeRTOS-Kernel port, and builds `pf_hello_world` for the selected `PICO_BOARD`. The TinyUSB configuration lives under `firmware/src/integration/usb`; USB descriptors, HID report helpers, and the statically allocated USB task live under `firmware/src/usb`. The composite device exposes one CDC ACM interface and one vendor-defined HID interface. Board topology assigns and initializes the monitor UART before the logger selects it for output; USB stdio stays disabled so log messages remain separate from CDC. The `middleware/logging` library provides serialized severity-tagged lines. The USB task services the [CDC CLI](cdc-cli.md) and publishes a versioned HID status report every 250 ms. HID Feature report 1 carries stable product, MCU, and USB serial identity; Input report 2 carries dynamic uptime, system clock, USB/CDC state, internal temperature, and LED state; Feature report 4 accepts LED set-state commands. Hosts read identity once per connection and receive changing values through the periodic input report. The temperature driver reads the on-chip ADC temperature sensor on channel 4, not GPIO 4. The onboard LED uses the selected SDK board's `PICO_DEFAULT_LED_PIN` (GPIO 25 for the current `pico` and `pico2` targets). Simple system, temperature, and LED drivers live flat under `firmware/src/drivers/`; the multi-file SDIO host is grouped under `drivers/sd_card/sdio_pio/`. The shared protocol is documented in [usb-protocol.md](usb-protocol.md). The development VID/PID is `0xCAFE:0x4011` and is not for product release. `firmware/src/main.c` initializes board UART and logging, temperature, and an available board LED, starts the USB task, and launches the scheduler. The PR workflow and `tools/validation/validate.sh` build the `pico` and `pico2` targets; physical HID, temperature, and LED behavior still require board validation.

Input Report 3 publishes SD-card detection/initialization state, selected bus mode, effective bus speed, CSD-derived capacity, and CID identity every second. Input Report 5 reports MSC ownership transitions, while Feature Report 5 requests enable/disable. In MSC mode the USB descriptor appends MSC after CDC and HID, preserving the PID and HID interface number; see [usb-protocol.md](usb-protocol.md). The owner gate rejects firmware block calls while the host owns the card. Enabling is rejected if FatFs is mounted or CLI storage work is pending; disabling is rejected until storage work is complete and the host ejects and allows removal. The Python, .NET, and WebHID dashboards expose the toggle and decode both reports.

CDC storage commands are queued to a statically allocated FreeRTOS storage worker,
which is the sole FatFs caller for this workflow. USB/CLI and storage communicate
through bounded request/reply queues; filesystem calls do not execute in the USB
task. MSC transitions are rejected while a storage command is outstanding. Files
are closed before command completion, and enabling MSC still requires explicit
filesystem unmount. See [the CDC CLI guide](cdc-cli.md) for commands, safeguards,
resource budgets, and validation limits.

This is a buildable starting point, not yet the complete application framework. The example currently uses Pico SDK and FreeRTOS APIs directly; future modules should establish the boundaries below before more hardware and application behavior is added.

HID Report 6 starts/cancels a large-file [SD self-test](sd-self-test.md) and
publishes verification progress and phase timings. Its RTOS-independent engine
lives beside storage middleware and runs in the same sole FatFs worker, with
exclusive ownership and create-new/cleanup safeguards. Browser, Python, and
.NET dashboards expose the controls and calculate average read/write metrics.

## Core and Runtime Policy

The initial runtime uses FreeRTOS-Kernel 11.3.1 with `configNUMBER_OF_CORES=1`. PicoForge starts its scheduler on core 0 and does not launch framework work on core 1. Core 1 is left unused by this baseline and may be assigned by a product application in a future, explicitly designed mode.

The FreeRTOS port itself is SMP-capable, but that does not mean this build runs SMP: the configured scheduler core count is one. Do not treat this as a hard real-time guarantee. Any core-1 application mode will need defined startup ownership, peripheral/IRQ affinity, cross-core synchronization, and measured timing.

The example uses static allocation for its application task and the port-provided idle-task memory. Software timers are enabled because the Pico port uses the timer-pend API for SDK synchronization interoperation. Additional tasks should document their stack, priority, ownership, and whether allocation is static or dynamic. The current firmware disables dynamic task allocation.

## Intended Dependency Direction

Application code orchestrates middleware and platform services. Middleware composes reusable drivers and platform services. Drivers and platform services adapt Pico SDK and board-specific hardware details.

```mermaid
flowchart TD
    App["Firmware application"] --> Middleware["Middleware services"]
    App --> Platform["Platform services"]
    Middleware --> Drivers["Reusable drivers"]
    Middleware --> Platform
    Drivers --> SDK["Pico SDK"]
    Platform --> SDK
    SDK --> Hardware["RP2040 / RP2350 hardware"]
```

Dependencies should flow toward the hardware-facing layers. Avoid circular dependencies between middleware, drivers, and platform services; use explicit interfaces when a lower layer needs an application-provided callback.

## Layer Responsibilities

### Application

Own product behavior, startup, and composition of the services needed by a firmware target. Keep policy and hardware register details out of application code where a reusable service or driver is appropriate.

### Middleware

Provide reusable application-facing services. Storage middleware mounts FatFs over the SD block-device API; application code remains responsible for supplying board-approved pins and choosing when to mount. The firmware currently uses the USB device role; a USB host role is not selected.

### Drivers

Encapsulate reusable peripheral operations and Pico SDK hardware APIs. Simple system, temperature, and LED drivers are flat under `firmware/src/drivers/`; multi-file drivers are grouped by subsystem under `firmware/src/drivers/sd_card/`. Storage and filesystem policy belongs in middleware. Keep board pin/power setup and controller resource selection in board support under `firmware/src/board/`. FatFs consumes the transport-neutral [SD-card block API](../firmware/src/drivers/sd_card/sd_card_block.h); transport configuration lives in [board support](../firmware/src/board/board.h). See the [hardware setup options](hardware-setup.md).

### SD-Card Driver Structure

The SD-card driver separates card-wide rules from transport-specific wire handling:

| Role | Shared | SDIO-PIO | SPI |
| --- | --- | --- | --- |
| Configuration and dispatch | `sd_card.c/h` | `sdio_pio.c/h` | `sd_spi.c/h` |
| Block contract and errors | `sd_card_block.h` | Same result type and operation suffixes | Same result type and operation suffixes |
| Protocol rules | `sd_card_protocol.c/h` | `sdio_pio_protocol.c/h` | `sd_spi_protocol.c/h` |
| Hardware engine | None | `sdio_pio_host.c/h` and `.pio` | Pico SDK SPI APIs |

Both backend headers expose `init`, `is_initialized`, `block_count`, `read_blocks`,
`write_blocks`, and `sync`, with the prefixes `pf_sdio_pio_` and `pf_sd_spi_`.
Configuration types remain distinct because PIO/DMA resources and SPI controllers
are not interchangeable. The SDIO block declarations now live in its backend
header, rather than a separate block header; the SPI prefix replaces `pf_sdspi_`.
Both backends return `pf_sd_card_result_t` directly.

The shared protocol library owns command CRC7/framing, CMD8 classification, CSD
capacity decoding, serial CRC16, sector-address conversion, and capacity/buffer
validation. SDIO helper entry points delegate card-wide algorithms to this library.
Native response parsing and the four-lane data CRC remain in the SDIO protocol;
SPI response tokens remain in the SPI protocol. Both transports accept any buffer
alignment and any sector count per call. Zero-sector transfers are no-ops after
readiness checks. SDIO reads use CMD18 batches of up to 32 sectors, with CMD17
for single-sector tails; writes use CMD25 for multi-sector runs and CMD24 for
single sectors. A CMD25 stream stays open after a write and is continued by a
write at the next sector, because FatFs issues one cluster per call and
restarting the stream for each cluster cuts throughput several-fold on the
tested card. Any read, non-contiguous write, sync (`CTRL_SYNC`), re-init or card
removal ends it with CMD12; the data is committed to the card only then, so
callers that need durability must sync. SPI transfers use synchronous single-sector operations. Both
transports require serialized access.

SDIO data moves by DMA between the PIO FIFOs and 520-byte wire images (512 data
bytes plus four lane CRC16s). Single-sector operations use an on-stack image;
multi-sector writes alternate two on-stack images to encode the next sector
while DMA and card programming complete for the current sector. Batched reads
use a static buffer for 16 images. The DMA channel byte-swaps each
word, so image byte order equals nibble transmission order. The CPU makes one
pass per sector to map lanes to GPIO order and compute or check the four CRCs,
which are held bit-sliced in one 64-bit register. SPI uses the CPU for the data
phase and a table-driven CRC16.

Long card waits (write busy, command-line busy) call an optional hook set with
`pf_sd_card_set_idle_hook()`, so an RTOS application can yield instead of spinning;
without a hook they poll. Repeating initialization restarts at the 400 kHz identification
clock. SPI pins must be the SCK/MOSI/MISO functions of the selected controller,
checked by both board validation and the driver.

Each transport has a backend library; the card facade links both, and both reuse
`pf_sd_card_protocol`. FatFs continues to depend only on the neutral block API;
its adapter stays in `integration/fatfs`, where it handles filesystem types and
status conversion.

### Platform

Own board support, configuration, logging, version metadata, and integration with the selected MCU and SDK. The board topology/configuration API lives in `firmware/src/board/`; board-specific pin mapping belongs here and must agree with [the hardware setup baseline](hardware-setup.md) and the board schematic. The selected Pico profile exposes SDIO-PIO and SPI wiring for explicit transport selection; physical card operation still needs board validation.

## External Components

Pico SDK 2.3.0 and FreeRTOS-Kernel V11.3.1 are pinned as Git submodules under `firmware/third_party/`. Initialize them with `git submodule update --init --recursive`. The build defaults to the SDK submodule, with `PICO_SDK_PATH` available as an override. The kernel's Pico SDK import selects the RP2040 or RP2350 port based on `PICO_PLATFORM`. The 4-bit PIO SD memory-card host is implemented in `firmware/src/drivers/sd_card/sdio_pio/`. FatFs R0.16 with upstream patches 1 and 2 is vendored under `firmware/third_party/fatfs/`, configured and adapted under `firmware/src/integration/fatfs/`, and compiled into the firmware.

Embedded CLI is also pinned as a Git submodule. Its middleware is independent
of the USB transport and RTOS; see [CDC CLI](cdc-cli.md) for the dependency pin,
session lifecycle, command-extension rules, memory budget, and validation plan.

`pf_storage_mount()` explicitly initializes the configured SD card and mounts the existing FAT/exFAT volume; it never formats the card. FatFs is built with exFAT and long-filename support enabled. The transport-neutral SD-card API uses 32-bit sector addresses (`FF_LBA64=0`), limiting volumes to fewer than 4,294,967,296 512-byte sectors (2 TiB); larger exFAT volumes are not supported. A local exFAT-image smoke test passed mount, create/write/sync, read/verify, delete, and unmount. No startup call currently selects a transport or mounts the volume. FatFs accesses media only through the transport-neutral SD-card block API and is configured without cross-task locking, so mount, unmount, and file operations must be serialized through one storage-owner task. Keep middleware independent of FreeRTOS task APIs where practical; put scheduler and port-specific integration behind the platform/runtime boundary.

## Testing and Validation

Ceedling is configured by the repository-root `project.yml`. Host unit tests belong under `firmware/tests/` and should mirror hardware-independent source modules in platform, drivers, and middleware. The configuration intentionally excludes `main.c` and board support from host builds.

`host/python/tests/`, `host/dotnet/tests/`, and `host/webhid/tests/` contain hardware-free host protocol/API tests. `tools/validation/validate.sh` checks shell syntax, validates the Ceedling project, runs firmware unit tests when present, and builds both MCU targets. Run the Python, .NET, and WebHID tests using the commands in the README. `tools/validation/coverage-firmware.sh` generates gcovr reports for firmware unit tests; it does not measure target timing or prove hardware behavior.

Physical wiring, pinout qualification, and tests requiring a board or probe belong in the hardware documentation and validation flow. The pinout document records standard Pico/Pico 2 targets and the SD breakout mappings. Board support can construct either transport configuration; SDIO-PIO and SPI card initialization and physical I/O still require board validation. External QSPI flash is not currently supported.