# Firmware Unit Tests

Ceedling 1.1.10 with Unity and CMock is configured by the repository-root `project.yml`. Tests target host-testable
application, board, driver, integration and middleware code, and mirror the corresponding subtree under
`firmware/src/`. The source/include root is `firmware/src/`; the Pico SDK-bound `main.c` is explicitly excluded from
host source collection.

Install Ruby 3 or newer and GCC/gcov, then run the setup wrapper from the repository root. The wrapper uses `uv` if available or Python 3.10+ with venv support to provision gcovr:

```sh
git submodule update --init firmware/third_party/embedded-cli firmware/third_party/levelx
tools/validation/setup-ceedling-testing.sh
```

Both source submodules are required even for Ceedling configuration validation; the setup wrapper does not fetch them.

The wrapper runs Ceedling from the repository root. To use `ceedling test:all` directly, add your user gem `bin`
directory to `PATH` and run it from the root. Name test files `test_<source_basename>[_<scenario>].c` beneath the matching
source subtree. For example, [sd_protocol.c](../src/drivers/sd_card/sd_protocol.c) is covered by
[test_sd_protocol.c](drivers/sd_card/test_sd_protocol.c) and
[test_sd_protocol_native.c](drivers/sd_card/test_sd_protocol_native.c). Keep related scenario tests alongside it.
Cross-layer tests live alongside the module that owns the behavior, not the lowest-level dependency they exercise.
SD firmware/USB ownership and reservation tests live under [app/storage](app/storage), named
`test_sd_media_freertos_serialization_*` and `test_sd_media_baremetal_*`. These exercise the application service
together with the raw driver and execution port for both SPI and SDIO; they are not standalone driver-port tests.
The layout contract in [test_validation_integrity.py](../../tests/test_validation_integrity.py) checks every Ceedling
test against a C module in the matching source directory. Scenario suffixes are allowed; they do not create a new
production module. Support fixtures and standalone smoke programs are not Ceedling test modules.

Run `tools/validation/coverage-firmware.sh` to execute `gcov:all` and generate HTML and Cobertura reports under `build/ceedling/artifacts/gcov/gcovr/`. Coverage requires at least one unit test.

## Host Behavior Regressions

C behavior regressions run through Ceedling/Unity, alongside the existing
protocol and middleware unit tests. Tests mirror the production source tree:

| Behavior | Test directory |
| --- | --- |
| Native/SPI initialization, transfers, CRC faults and recovery | [drivers/sd_card](drivers/sd_card) |
| Physical SPI, SDIO-PIO and QSPI-PIO lifecycle, dispatch, wire operations and interrupt-wait races | [drivers/storage](drivers/storage) |
| FreeRTOS SD reservation and application ownership integration | [app/storage](app/storage), `test_sd_media_freertos_serialization_*` |
| SD media ownership and byte transfers with the bare-metal port | [app/storage](app/storage), `test_sd_media_baremetal_*` |
| Media configuration, owner-gated I/O, retained diagnostics, commands, file benchmark, automount and recovery | [app/storage](app/storage) |
| Singleton volume lifecycle and format telemetry | [app/storage](app/storage) |
| Product CLI prompt, built-ins, completion and history | [app/cli](app/cli) |
| Logical blocks, SD adaptation, and FatFs disk results/geometry/protection | [middleware/storage](middleware/storage) |
| USB memory reports, self-test admission and MSC callback behavior | [app/usb](app/usb) |
| Interval CPU accounting, first-sample availability and 64-bit runtime deltas | [integration/freertos](integration/freertos) |

The volume lifecycle test is [test_fatfs_volume.c](app/storage/test_fatfs_volume.c), matching
[fatfs_volume.c](../src/app/storage/fatfs_volume.c). It binds a caller-supplied block device through
`pf_fatfs_volume_*` APIs without SD initialization or driver mocks. Application command/automount tests cover
SD initialization failure before binding, and already-mounted commands avoid reinitializing live media.
[Block-device tests](middleware/storage/test_block_device.c) reject each missing callback and test the transfer-byte
ceiling on a real 32-bit host build. The latter is explicitly ignored on 64-bit hosts. On x86 Linux with GCC multilib
and 32-bit libc development support installed, run it without changing the normal Ceedling configuration:

```sh
ceedling --mixin='={project: {build_root: build/ceedling-block32}, flags: {test: {compile: {test_block_device: [-m32]}, link: {test_block_device: [-m32]}}}}' test:test_block_device
```

The callbacks only record dispatch, so the byte-count boundary test does not allocate or access gigabytes of memory.

Shared CRC7, command-frame, CSD and address-conversion tests live in
[test_sd_protocol.c](drivers/sd_card/test_sd_protocol.c); native wire-codec tests remain in
[test_sd_protocol_native.c](drivers/sd_card/test_sd_protocol_native.c).
Both fused-copy variants compile the same consolidated protocol implementation in their
[disabled](drivers/sd_card/test_sd_protocol_copy_fused0.c) and
[enabled](drivers/sd_card/test_sd_protocol_copy_fused1.c) tests.
Bare-metal artifact checks also verify that SPI links no native codecs or CRC tables, and that the native fused-copy
option reaches the consolidated protocol compilation.
[SPI behavior tests](drivers/sd_card/test_sd_spi.c) inject selection failures across command, register, single-block,
multi-block and sync paths, requiring zero subsequent bus activity. Native tests verify one-time callback binding
and preserved stream recovery. Optional wait setup is tested with
[SD media/FreeRTOS reservation integration](app/storage/test_sd_media_freertos_serialization_sdio.c).
Storage tests cover raw phase validation before CS, arbitrary released-line clock counts/deadlines, short/long native
response byte packing and partial failure capture. NOR adapter tests verify opcode/address construction and payload
bit-count overflow independently of the physical engines. The SD host test retains the exact 80-clock, low-speed
initialization check.
Common lifecycle tests live in [test_bus.c](drivers/storage/test_bus.c), raw serial dispatch
tests in [test_serial.c](drivers/storage/test_serial.c), and native dispatch tests in
[test_sd_wire.c](drivers/storage/test_sd_wire.c). Concrete backend tests remain under their corresponding storage
subdirectories. Cross-layer HID/card-unavailable scenarios are owned by
[test_usb_hid_sd_responsiveness.c](app/usb/test_usb_hid_sd_responsiveness.c), matching the HID source module.

Run all C behavior tests with:

```sh
ceedling test:all
```

Use `ceedling test:TEST_FILE_NAME` for one test file, without the `.c` suffix.
`tools/validation/validate.sh` invokes the same Ceedling suite, plus shell
syntax checks, firmware builds and the remaining Python validation.

Reusable hardware/RTOS fakes and private translation-unit fixtures live under
[support](support). Tests select scoped fake-header paths with
`TEST_INCLUDE_PATH`; they do not add fake SDK headers to firmware builds.
Standalone firmware smoke programs stay under [ports](ports); they are not host unit tests.
Where private state or static functions must be exercised, fixtures include
the production translation unit rather than copying functions out of source
strings. Assertions use Unity so failures appear in Ceedling results.

Project-level contracts, PIO instruction models and validation-tool regression
tests live under the root [tests/](../../tests/) directory, along with build-artifact
tests, not alongside validation tooling. See
[test ownership and commands](../../tests/README.md) for their roles and requirements.
These checks complement, rather than replace, C behavior tests.

Board clock defaults and executed RX/DMA setup, overlap, and cleanup sequencing
are Ceedling tests, not Python source-text checks. Dated test records retain
historical command names.

Host fakes do not execute physical GPIO, DMA, interrupt scheduling or USB BOT
traffic. Electrical timing, sustained card transfers, real interrupt behavior,
and board/probe interactions still require hardware validation. Passing the
host suite does not qualify a card at 50 MHz.

Raw NOR protocol tests live under [drivers/nor_flash](drivers/nor_flash).
See [external NOR support](../../docs/nor-flash.md#tests-and-remaining-qualification)
for the standalone SPI/QSPI smoke builds, PIO model and hardware-test boundaries.

[LevelX integration tests](integration/levelx/test_levelx_nor.c) run the pinned
upstream NOR engine on a simulated flash using `ceedling test:test_levelx_nor`.
Initialize `firmware/third_party/levelx` first. See
[the LevelX port guidance](../../docs/nor-flash.md#levelx-nor-port) for configuration,
region ownership, failure recovery and the limits of host fault-injection tests.
Build-selected NOR filesystem and MSC tests are described in the
[NOR backend test commands](../../docs/nor-flash.md#build-selected-fatfs-and-usb-msc), including real FatFs/LevelX
file persistence over simulated flash and owner-gated USB handoff tests.

Application CLI tests compile the pinned Embedded CLI submodule, so initialize
submodules before running Ceedling. See [the CDC CLI validation notes](../../docs/cdc-cli.md#validation)
for the focused command and board-only checks.