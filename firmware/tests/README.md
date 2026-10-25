# Firmware Unit Tests

Ceedling 1.1.10 with Unity and CMock is configured by the repository-root `project.yml`. Tests target host-testable
application, board, driver, integration and middleware code, and mirror the corresponding subtree under
`firmware/src/`. The source/include root is `firmware/src/`; the Pico SDK-bound `main.c` is explicitly excluded from
host source collection.

Install Ruby 3 or newer and GCC/gcov, then run the setup wrapper from the repository root. The wrapper uses `uv` if available or Python 3.10+ with venv support to provision gcovr:

```sh
git submodule update --init firmware/third_party/embedded-cli
tools/validation/setup-ceedling-testing.sh
```

The source submodule is required even for Ceedling configuration validation; the setup wrapper does not fetch it.

The wrapper runs Ceedling from the repository root. To use `ceedling test:all` directly, add your user gem `bin`
directory to `PATH` and run it from the root. Name test files `test_<source_basename>[_<scenario>].c` beneath the matching
source subtree. For example, [sd_protocol.c](../src/drivers/sdcard/sd_protocol.c) is covered by
[test_sd_protocol.c](drivers/sdcard/test_sd_protocol.c) and
[test_sd_protocol_native.c](drivers/sdcard/test_sd_protocol_native.c). Keep related scenario tests alongside it.
Cross-layer tests live alongside the module that owns the behavior, not the lowest-level dependency they exercise.
SD firmware/USB ownership and reservation tests live under [middleware/storage](middleware/storage), named
`test_storage_media_freertos_serialization_*` and `test_storage_media_baremetal_*`. These exercise the storage service
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
| Native/SPI initialization, transfers, CRC faults and recovery | [drivers/sdcard/link](drivers/sdcard/link), [sdio_pio](drivers/sdcard/sdio_pio), [spi](drivers/sdcard/spi) |
| Physical SPI and SDIO-PIO lifecycle, dispatch, wire operations and interrupt-wait races | [drivers/sdcard/link](drivers/sdcard/link), [sdio_pio](drivers/sdcard/sdio_pio), [spi](drivers/sdcard/spi) |
| FreeRTOS SD reservation and storage ownership integration | [middleware/storage](middleware/storage), `test_storage_media_freertos_serialization_*` |
| SD media ownership and byte transfers with the bare-metal port | [middleware/storage](middleware/storage), `test_storage_media_baremetal_*` |
| Media configuration, owner-gated I/O and neutral status | [middleware/storage](middleware/storage) |
| Commands, file benchmark, automount and recovery through the worker | [app/storage](app/storage) |
| Singleton volume lifecycle and format telemetry | [middleware/storage](middleware/storage) |
| Product CLI prompt, built-ins, completion and history | [app/cli](app/cli) |
| Boot-config record validation and platform flash persistence | [config](config), [platform](platform) |
| Logical blocks, FatFs disk results/geometry/protection, and volume lifecycle | [middleware/storage](middleware/storage) |
| USB memory reports, self-test admission and MSC callback behavior | [app/usb](app/usb) |
| Interval CPU accounting, first-sample availability and 64-bit runtime deltas | [integration/freertos](integration/freertos) |

The volume lifecycle test is [test_fatfs_volume.c](middleware/storage/test_fatfs_volume.c), matching
[fatfs_volume.c](../src/middleware/storage/fatfs_volume.c). It binds a caller-supplied block device through
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
[test_sd_protocol.c](drivers/sdcard/test_sd_protocol.c); native wire-codec tests remain in
[test_sd_protocol_native.c](drivers/sdcard/test_sd_protocol_native.c).
Both fused-copy variants compile the same consolidated protocol implementation in their
[disabled](drivers/sdcard/test_sd_protocol_copy_fused0.c) and
[enabled](drivers/sdcard/test_sd_protocol_copy_fused1.c) tests.
Bare-metal artifact checks also verify that SPI links no native codecs or CRC tables, and that the native fused-copy
option reaches the consolidated protocol compilation.
[SPI behavior tests](drivers/sdcard/test_sd_spi.c) inject selection failures across command, register, single-block,
multi-block and sync paths, requiring zero subsequent bus activity. Native tests verify one-time callback binding
and preserved stream recovery. Optional wait setup is tested with
[SD media/FreeRTOS reservation integration](middleware/storage/test_storage_media_freertos_serialization_sdio.c).
Storage tests cover raw phase validation before CS, arbitrary released-line clock counts/deadlines, short/long native
response byte packing and partial failure capture. The SD host test retains the exact 80-clock, low-speed
initialization check.
Common lifecycle tests live in [test_sd_link.c](drivers/sdcard/link/test_sd_link.c), raw serial dispatch
tests in [test_sd_link_serial.c](drivers/sdcard/link/test_sd_link_serial.c), and native dispatch tests in
[test_sd_link_wire.c](drivers/sdcard/link/test_sd_link_wire.c). Concrete backend tests remain under their corresponding backend
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

The [USB DCD guard tests](integration/usb/test_dcd_guard.c) exercise fixed-core IRQ forwarding, task-side calls on
either core, nesting, return-value forwarding and concurrent host threads using a simulated hardware lock.
They also check copied FIFO event delivery in task context, full-capacity batches, SOF coalescing and bounded
dispatch when new events arrive during callbacks.
The pinned TinyUSB surface/queue contract lives in
[`test_tinyusb_dcd_contract.py`](../../tests/contracts/test_tinyusb_dcd_contract.py); it detects missing wrappers
when the upstream DCD entry surface changes. SDIO wait tests also force a completion IRQ to contend with timeout
cleanup on a second host thread. Run the focused synchronization checks with:

```bash
ceedling 'test:pattern[(dcd_guard|sdio_wait_freertos|fatfs_volume)]'
python3 -m pytest tests/contracts/test_tinyusb_dcd_contract.py -q
```

These checks validate integration synchronization, not the real controller or an SMP-enabled firmware image.

Application CLI tests compile the pinned Embedded CLI submodule, so initialize
submodules before running Ceedling. See [the CDC CLI validation notes](../../docs/design/usb/usb-cdc-cli.md#validation)
for the focused command and board-only checks.