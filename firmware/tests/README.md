# Firmware Unit Tests

Ceedling 1.1.10 with Unity and CMock is configured by the repository-root `project.yml`. Tests should target hardware-independent platform, driver, and middleware code, and mirror the corresponding subtree under `firmware/src/`. The source/include root is `firmware/src/`; the Pico SDK-bound `main.c` is explicitly excluded from host source collection.

Install Ruby 3 or newer and GCC/gcov, then run the setup wrapper from the repository root. The wrapper uses `uv` if available or Python 3.10+ with venv support to provision gcovr:

```sh
git submodule update --init firmware/third_party/embedded-cli firmware/third_party/levelx
tools/validation/setup-ceedling-testing.sh
```

Both source submodules are required even for Ceedling configuration validation; the setup wrapper does not fetch them.

The wrapper runs Ceedling from the repository root. To use `ceedling test:all` directly, add your user gem `bin`
directory to `PATH` and run it from the root. Name test files `test_<source_basename>[_<scenario>].c` beneath the matching
source subtree. For example, [sd_native_protocol.c](../src/drivers/sd_card/sd_native_protocol.c) is covered by
[test_sd_native_protocol.c](drivers/sd_card/test_sd_native_protocol.c). Keep related scenario tests alongside it.
FreeRTOS port tests live under [ports/freertos](drivers/sd_card/ports/freertos), and bare-metal port tests under
[ports/baremetal](drivers/sd_card/ports/baremetal), matching their production port directories.

Run `tools/validation/coverage-firmware.sh` to execute `gcov:all` and generate HTML and Cobertura reports under `build/ceedling/artifacts/gcov/gcovr/`. Coverage requires at least one unit test.

## Host Behavior Regressions

C behavior regressions run through Ceedling/Unity, alongside the existing
protocol and middleware unit tests. Tests mirror the production source tree:

| Behavior | Test directory |
| --- | --- |
| Native/SPI initialization, transfers, CRC faults and recovery | [drivers/sd_card](drivers/sd_card) |
| Physical SPI, SDIO-PIO and QSPI-PIO lifecycle, dispatch, wire operations and interrupt-wait races | [drivers/storage](drivers/storage) |
| FreeRTOS facade ownership and serialization | [drivers/sd_card/ports/freertos](drivers/sd_card/ports/freertos) |
| Bare-metal facade and port behavior | [drivers/sd_card/ports/baremetal](drivers/sd_card/ports/baremetal) |
| Card presence, automount and recovery | [middleware/storage](middleware/storage) |
| USB memory reports, self-test admission and MSC callback behavior | [usb](usb) |

Shared CRC7, command-frame, CSD and address-conversion tests live in
[test_sd_protocol.c](drivers/sd_card/test_sd_protocol.c); native wire-codec tests remain in
[test_sd_native_protocol.c](drivers/sd_card/test_sd_native_protocol.c).
[SPI behavior tests](drivers/sd_card/test_sd_spi.c) inject selection failures across command, register, single-block,
multi-block and sync paths, requiring zero subsequent bus activity. Native tests verify one-time callback binding
and preserved stream recovery. Optional wait setup is tested with
[FreeRTOS port serialization](drivers/sd_card/ports/freertos/test_sd_port_serialization_sdio.c).

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

CLI middleware tests compile the pinned Embedded CLI submodule, so initialize
submodules before running Ceedling. See [the CDC CLI validation notes](../../docs/cdc-cli.md#validation)
for the focused command and board-only checks.