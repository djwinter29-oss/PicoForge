# Contributing

Thanks for helping improve PicoForge. Contributions should keep changes focused, follow the existing layer boundaries, and distinguish implemented behavior from planned work.

## Before Opening a Pull Request

- Search existing issues and pull requests before starting a large or behavior-changing contribution.
- For a substantial change, open an issue or discussion first so scope and hardware assumptions can be agreed.
- Do not assume board GPIO assignments. Update the board topology and `docs/guides/hardware-setup.md` only from an identified board schematic and verified wiring.
- Keep board-specific configuration in `firmware/src/config/board.c`, reusable drivers under `firmware/src/drivers/`, FatFs/Pico SDK integration under `firmware/src/integration/`, and application-facing services under `firmware/src/middleware/`.
- Keep simple drivers flat in `firmware/src/drivers/`; use a named subdirectory for a multi-file or substantial driver such as SDIO.
- Preserve third-party provenance and licenses. Do not edit vendored code except for a documented, necessary patch.
- Use include-directory-rooted paths in project-owned C headers, sources, and test fixtures, not `./` or `../`
  traversal. Cross-module production includes use paths rooted at `firmware/src`; test fixtures use the configured
  test-support include directories. Same-directory includes may keep the filename alone.

## Build and Test

Firmware changes require the Pico SDK and FreeRTOS submodules:

```sh
git submodule update --init --recursive
tools/validation/validate.sh
```

The validation script checks shell syntax, runs available Ceedling tests, and builds both `pico` and `pico2`. For host-only firmware unit tests, run `tools/validation/setup-ceedling-testing.sh test:all`; for coverage, run `tools/validation/coverage-firmware.sh`.

Host application tests are hardware-free and run with:

```sh
python3 -m pip install --editable host/python
tools/validation/test-host.sh
```

The host test script requires Python 3.10+, Node.js 22+, and .NET 10. Set `PYTHON`, `NODE`, or `DOTNET` when using non-default executable paths.

Add focused tests for non-trivial hardware-independent behavior. Physical SD-card, USB enumeration, GPIO, timing, and temperature behavior require an identified board and must not be claimed from host tests or cross-builds alone.

## Pull Requests

- Explain the behavior change, affected targets, and any board assumptions.
- Include focused tests and the commands/results used to validate the change.
- Update the authoritative documentation when public behavior, configuration, protocol, or hardware wiring changes.
- Keep generated build output, Python bytecode, secrets, and unrelated formatting changes out of the pull request.
- Call out checks that require hardware and have not been run.
