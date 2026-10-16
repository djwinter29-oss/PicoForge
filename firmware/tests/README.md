# Firmware Unit Tests

Ceedling 1.1.10 with Unity and CMock is configured by the repository-root `project.yml`. Tests should target hardware-independent platform, driver, and middleware code, and mirror the corresponding subtree under `firmware/src/`. The source/include root is `firmware/src/`; the Pico SDK-bound `main.c` is explicitly excluded from host source collection.

Install Ruby 3 or newer and GCC/gcov, then run the setup wrapper from the repository root. The wrapper uses `uv` if available or Python 3.10+ with venv support to provision gcovr:

```sh
tools/validation/setup-ceedling-testing.sh
```

The wrapper runs Ceedling from the repository root. To use `ceedling test:all` directly, add your user gem `bin` directory to `PATH` and run it from the root. Name test files `test_*.c` and place them beneath the matching source subtree; for example, `firmware/src/drivers/sd_card/sdio_pio/sdio_pio_protocol.c` is covered by `firmware/tests/drivers/sd_card/sdio_pio/test_sdio_pio_protocol.c`.

Run `tools/validation/coverage-firmware.sh` to execute `gcov:all` and generate HTML and Cobertura reports under `build/ceedling/artifacts/gcov/gcovr/`. Coverage requires at least one unit test.

## Host Behavior Regressions

C behavior regressions run through Ceedling/Unity, alongside the existing
protocol and middleware unit tests. Tests mirror the production source tree:

| Behavior | Test directory |
| --- | --- |
| SDIO initialization, read batches, clock recovery, TX and interrupt-wait races | [drivers/sd_card/sdio_pio](drivers/sd_card/sdio_pio) |
| SPI transactions, token/CRC faults and uncertain completion | [drivers/sd_card/spi](drivers/sd_card/spi) |
| Facade ownership, serialization and bare-metal ports | [drivers/sd_card](drivers/sd_card) |
| Card presence, automount and recovery | [middleware/storage](middleware/storage) |
| USB memory reports, self-test admission and MSC callback behavior | [usb](usb) |

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
Where private state or static functions must be exercised, fixtures include
the production translation unit rather than copying functions out of source
strings. Assertions use Unity so failures appear in Ceedling results.

Python remains appropriate for assembling and modeling PIO instructions,
checking pinned vendor-source ordering, inspecting ELF/build artifacts, and
testing those Python validation tools. These are distinct from C behavior
tests and are not substitutes for them.

Host fakes do not execute physical GPIO, DMA, interrupt scheduling or USB BOT
traffic. Electrical timing, sustained card transfers, real interrupt behavior,
and board/probe interactions still require hardware validation. Passing the
host suite does not qualify a card at 50 MHz.

CLI middleware tests compile the pinned Embedded CLI submodule, so initialize
submodules before running Ceedling. See [the CDC CLI validation notes](../../docs/cdc-cli.md#validation)
for the focused command and board-only checks.