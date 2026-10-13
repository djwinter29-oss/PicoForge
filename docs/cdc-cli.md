# USB CDC CLI

USB CDC ACM is the interactive command interface. UART remains the independent
debug-log transport; neither HID telemetry nor MSC ownership is changed by CLI
commands. The periodic CDC greeting has been removed.

## Dependency

[Embedded CLI](https://github.com/funbiscuit/embedded-cli) is an MIT-licensed Git
submodule at `firmware/third_party/embedded-cli`, pinned to v0.1.4, commit
`ffa8014c6230ba809c6a655ccc611a6c4689113f`. Its license remains in the submodule's
`LICENSE.txt`. Firmware compiles the upstream C source directly, not the upstream
examples or test dependencies. Initialize dependencies from the repository root:

```sh
git submodule update --init --recursive
```

## Terminal

Open the PicoForge CDC port, not the Debug Probe UART port. On Linux, identify it
under `/dev/serial/by-id/` by its `PicoForge` product name. A terminal must assert
DTR and should disable local echo. For example:

```sh
screen /dev/ttyACM1 115200
```

The device number can vary. CDC line-coding baud rate does not set a physical
UART speed. Opening the port produces `PicoForge> `. Enter sends a command; Tab
completes command names, Backspace edits input, and arrow keys navigate history
and the current line. Live completion is disabled; Tab completion is enabled.
CR, LF, and CRLF are accepted.

| Command | Behavior |
| --- | --- |
| `help` | List commands; `help <command>` displays command help. |
| `version` | Show the PicoForge CLI and Embedded CLI version. |
| `echo <text>` | Echo text for non-destructive transport checks. |

Commands do not read or write the SD card, mount a filesystem, or change MSC
ownership. Closing CDC or USB re-enumeration discards the session's partial
command, queued output, and history. Reopening starts a fresh prompt.

## Integration

[CLI middleware](../firmware/src/middleware/cli/cli.c) owns the static Embedded
CLI instance and command bindings. It has no TinyUSB, board, or FreeRTOS
dependency. Its output callback and input/process API are documented in
[cli.h](../firmware/src/middleware/cli/cli.h). Add command bindings during
`pf_cli_init()` using `embeddedCliAddBinding()`; argument-tokenization helpers
come from the upstream framework. Callbacks execute in the USB task, so keep
them bounded and non-blocking; do not run long operations there.

[The USB task](../firmware/src/usb/task.c) is the sole owner of CDC and the CLI.
The CLI uses a 4 KiB aligned static arena, 64-byte RX buffer, 128-byte command
buffer, 256-byte history buffer, and eight application-binding slots. The CDC
adapter has a 2 KiB output ring. It processes at most 32 received characters
per task iteration and pauses input when less than 1 KiB of output space remains.
It never waits for the host to consume output. Each process call must emit at
most 1 KiB; adding commands requires preserving that output budget. Exceeding the
ring fails explicitly rather than silently dropping output. HID/MSC continue to
be serviced by the same task.

## Validation

Run the hardware-independent middleware tests:

```sh
tools/validation/setup-ceedling-testing.sh test:test_cli
```

They cover prompt/help, commands, invalid arguments, unknown commands, session
reset, completion/history, and the output reserve. They do not verify USB CDC
enumeration or host backpressure.

For a board check, safely eject and disable any active MSC drive before flashing.
Open CDC and run `help`, `version`, and `echo CDC test`. Close/reopen to verify a
fresh prompt, and check that HID reports continue during commands. Stop reading
CDC briefly, resume, and confirm output arrives without a board reset. No
formatting, filesystem changes, or card writes are needed for this check.