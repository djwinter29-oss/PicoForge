# Firmware Build Configuration

[Documentation](../README.md) / [Reference](README.md)

Use this reference to select firmware components and choose the checks for that configuration.
Run commands from the repository root after initializing the pinned submodules. Toolchain setup and normal
build commands belong in [firmware development](../development/firmware-testing.md).

## Optional Modules

Optional modules are compiled in or out. A disabled module adds no code, RAM buffers or third-party stacks, and its
HID capability bit is cleared so dashboards disable its controls.

| Module | CMake option | `build.sh` flag | Default | When OFF |
| --- | --- | --- | --- | --- |
| Storage benchmark | `PF_ENABLE_STORAGE_BENCHMARK` | `--benchmark ON\|OFF` | CMake OFF; wrapper ON | Capability bit 5 clear; opcode 4 returns Unsupported; 32 KiB buffer omitted. |
| Wireless (Wi-Fi, BLE, TCP CLI, mDNS) | `PF_ENABLE_WIRELESS` | `--wireless ON\|OFF` | ON | Capability bit 7 clear; CYW43, lwIP, BTstack and mbedTLS are not linked. |

`PF_ENABLE_WIRELESS` only has an effect on CYW43 boards (`pico_w`, `pico2_w`); other boards never link the radio.
The W-board LED is wired to the radio chip, so a radio-free W image also clears capability bit 0 (LED) and rejects
LED commands. Image-size savings depend on the board, build type, and other selected modules; no single size
reduction is guaranteed.

The wrapper reads `PF_ENABLE_STORAGE_BENCHMARK` and `PF_ENABLE_WIRELESS` from the environment when the flag is absent,
accepts only `ON` or `OFF`, and passes both options on every run, so a reused build directory cannot keep a stale
selection. `tools/firmware/load.sh` forwards `--benchmark` to its build step.

```sh
tools/firmware/build.sh --board pico2_w --benchmark OFF --wireless OFF --build-dir build/pico2_w-minimal
```

Use a separate `--build-dir` per variant to keep both images. Direct CMake accepts the same options:

```sh
cmake -S firmware -B build/pico2_w-minimal -G Ninja -DPICO_BOARD=pico2_w \
  -DPF_ENABLE_STORAGE_BENCHMARK=OFF -DPF_ENABLE_WIRELESS=OFF
cmake --build build/pico2_w-minimal
```

Radio and TCP CLI switches in [saved boot settings](settings.md) only stop services at runtime; they do not shrink the
image. Use `--wireless OFF` when the radio code itself must be absent.

## Core Components

USB CDC, vendor HID, MSC, storage, the CLI, logging, LED and temperature services are always built. The USB interface
numbers and HID reports are a fixed host contract, and MSC ownership, forced save and CLI storage commands depend on
each other, so these are not independent modules.

## Hardware and Tuning Options

| Choice | Control | Default and limits |
| --- | --- | --- |
| Board/MCU | `PICO_BOARD=pico`, `pico_w`, `pico2`, or `pico2_w` | SDK board selection supplies hardware support; the wrapper defaults to `pico`. |
| SD transport | `PF_SDCARD_TRANSPORT=SDIO_PIO` or `SPI` | `SDIO_PIO`; exactly one protocol backend, physical engine and board binding is linked. |
| SD execution port | `PF_SDCARD_PORT=FREERTOS` or `BAREMETAL` | `FREERTOS`; bare metal builds a driver smoke target, not the USB/filesystem application. |
| Contiguous benchmark allocation | `PF_STORAGE_SELF_TEST_PREALLOCATE=ON` or `OFF` | OFF; relevant only when the benchmark is enabled. |
| Fused SDIO CRC/copy | `PF_SDIO_PIO_FUSED_COPY=ON` or `OFF` | OFF; SDIO-only experimental optimization. |
| SDIO interrupt waits | `PF_SDCARD_INTERRUPT_WAIT=ON` or `OFF` | OFF; requires SDIO and the FreeRTOS port. Invalid combinations fail configuration. |

All options live in [CMakeLists.txt](../../firmware/CMakeLists.txt). The CPU clock and storage-speed target are saved
boot settings, not build options. `PF_SYS_CLK_MHZ` and `PF_SDIO_PIO_HIGH_SPEED` are retired and rejected by direct
CMake configuration; for an older build directory add `-U PF_SYS_CLK_MHZ -U PF_SDIO_PIO_HIGH_SPEED` once. The wrapper
clears them automatically. Flash the resulting UF2 as described in
[getting started](../getting-started.md#2-flash-the-board).

## Validate the Selected Variant

| Change | Smallest relevant checks |
| --- | --- |
| Benchmark behavior | `tools/validation/setup-ceedling-testing.sh test:test_file_benchmark`; build benchmark ON and OFF. |
| Optional module gating | `test:test_usb_hid_modules_disabled`, `tests/contracts/test_led_runtime.py`, `tests/contracts/test_firmware_version.py`; PR CI builds every board/transport with both modules OFF. |
| SD protocol or transport | Focused mirrored C tests, then build both SPI and SDIO on the affected MCU. |
| Bare-metal port | [Driver smoke build and artifact checks](sdcard/driver.md#execution-environment-ports); do not treat it as an application image. |
| Fused copy or IRQ waits | [Controlled comparison procedure](sdcard/driver.md#performance-controls-and-status), including both option states; IRQ waits also require synchronization tests. |
| USB-visible capabilities | Host report/control tests and the affected firmware variants; benchmark-free builds must disable unsupported controls. |

Test placement and required artifact inputs are defined in the [test ownership index](../../tests/README.md).
The [hardware acceptance procedures](../development/firmware-testing.md#hardware-validation) remain necessary for
physical behavior. A successful configuration or cross-build does not qualify an optional optimization.
