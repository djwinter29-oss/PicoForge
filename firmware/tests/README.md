# Firmware Unit Tests

Ceedling 1.1.10 with Unity and CMock is configured by the repository-root `project.yml`. Tests belong directly in this directory and should target hardware-independent platform, driver, and middleware code. The source/include root is `firmware/src/`; the Pico SDK-bound `main.c` is explicitly excluded from host source collection.

Install Ruby 3 or newer and GCC/gcov, then run the setup wrapper from the repository root. The wrapper uses `uv` if available or Python 3.10+ with venv support to provision gcovr:

```sh
tools/firmware/setup-ceedling-testing.sh
```

The wrapper runs Ceedling from the repository root. To use `ceedling test:all` directly, add your user gem `bin` directory to `PATH` and run it from the root. Name test files `test_*.c` and place them in this directory; for example, `firmware/tests/test_storage.c` is discovered by the root project configuration.

Run `tools/validation/coverage-firmware.sh` to execute `gcov:all` and generate HTML and Cobertura reports under `build/ceedling/artifacts/gcov/gcovr/`. Coverage requires at least one unit test. Tests of SDK timing, GPIO, USB, SDIO, or interrupts still belong in target or hardware validation.