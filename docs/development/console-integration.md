# Console Integration

This reference owns the Embedded CLI dependency, command registration, CDC session lifecycle, buffers, and
software validation. User commands and stalled-output recovery remain in [the console guide](../guides/console.md).

## Dependency

[Embedded CLI](https://github.com/funbiscuit/embedded-cli) is an MIT-licensed Git
submodule at `firmware/third_party/embedded-cli`, pinned to v0.1.4, commit
`ffa8014c6230ba809c6a655ccc611a6c4689113f`. Its license remains in the submodule's
`LICENSE.txt`. Firmware compiles the upstream C source through the
[input-pump adapter](../../firmware/src/integration/cli/embedded_cli_adapter.c), not the upstream examples or test dependencies.
The adapter reuses the pinned editor/parser internals because the public API has no editor-overflow rejection hook.
It rejects editor or RX overflow through the next line ending, before dispatch, without changing vendor files.
RX overflow discards the queued backlog because its line endings cannot identify which command lost bytes.
Review the adapter's flag allocation and input-pump semantics when updating the dependency.
Initialize dependencies from the repository root:

```sh
git submodule update --init --recursive
```

## Extending commands

Keep device command grammar and formatting in `app/cli`, transport/session queues in `app/usb/usb_task`, and hardware,
configuration and storage operations in their owning services. CLI dispatch runs on the USB owner task for both CDC
and TCP. Never perform FatFs operations directly there; submit them to the sole storage worker.
Validate a complete request before changing state, keep each synchronous response within the 1024-byte output reserve,
and report request admission separately from asynchronous completion. Full top-level help must fit that reserve too.
The transport pauses CLI dispatch during pending HID save/reboot or benchmark admission, and services CDC/Wi-Fi while
draining reboot acknowledgements. Add command contract tests in `firmware/tests/app/cli`, plus ownership/transport
regressions when those boundaries change.

```sh
tools/validation/setup-ceedling-testing.sh test:cli test:cli_device test:usb_hid_wireless
```

## Integration

[Application CLI](../../firmware/src/app/cli/cli.c) owns the static Embedded
CLI instance and command bindings. It has no TinyUSB, board, or FreeRTOS
dependency. Its output callback and input/process API are documented in
[cli.h](../../firmware/src/app/cli/cli.h). Add session bindings using
`pf_cli_add_binding()` after initialization; argument-tokenization helpers
come from the upstream framework. Callbacks execute in the USB task, so keep
them bounded and non-blocking; do not run long operations there.

[The USB task](../../firmware/src/app/usb/usb_task.c) is the sole owner of CDC and the CLI; the Wi-Fi
[TCP transport](../../firmware/src/app/wireless/wifi/tcp_cli.c) is polled by the same task, so the session pump
treats CDC and TCP as interchangeable byte streams.
The CLI uses a 4 KiB aligned static arena, 64-byte RX buffer, 128-byte command
buffer (126 editable bytes plus two tokenizer terminators), 256-byte history buffer, and five application-binding slots. The session
has a 2 KiB output ring. It processes at most 32 received characters
per task iteration and pauses input when less than 1 KiB of output space remains.
It never waits for the host to consume output. Each process call must emit at
most 1 KiB; adding commands requires preserving that output budget. Exceeding the
ring fails explicitly rather than silently dropping output. HID/MSC continue to
be serviced by the same task.

[The static storage worker](../../firmware/src/app/storage/storage_worker.c)
is the sole FatFs caller for CLI operations. It uses an 8 KiB stack, a one-request
queue, and eight 256-byte reply slots. [The command dispatcher](../../firmware/src/app/cli/cli_storage.c)
has no RTOS or USB dependency; the USB task registers it with the worker at startup. The worker sends bounded lines through the queue;
only the USB task calls CLI print APIs. Replies carry a session ID so old output
does not leak into a newly opened terminal. Backpressure suspends the worker,
not the USB task. Disconnect cancellation releases read-only output backpressure;
accepted mutations are not abandoned midway. MSC transitions are rejected while
the worker is busy; enabling from a mounted filesystem queues worker unmount before ownership transfer. New application
filesystem clients must also be routed through this worker, not a second task.

## Validation

Run the hardware-independent platform tests:

```sh
tools/validation/setup-ceedling-testing.sh test:cli test:cli_device test:cli_storage
```

They cover prompt/help, commands, invalid arguments, unknown commands, session
reset, completion/history, asynchronous binding registration, and the output
reserve, editor/RX overflow rejection, cursor/history behavior at the input limit, and raw configuration whitespace.
Mocked FatFs tests cover commands, ownership/mount guards, safe paths,
create-new/append modes, close/sync and partial-write errors, bounded cat output,
directory cleanup, UTF-8-safe marked filename shortening, confirmed deletion, and cancellation. They do not verify actual
card I/O, FreeRTOS queue scheduling, USB CDC enumeration, or host backpressure.

For a board check, safely eject and disable any active MSC drive before flashing.
Open CDC and run `help` and `board get version`. Close/reopen to verify a
fresh prompt, and check that HID reports continue during commands. Stop reading
CDC briefly, resume, and confirm output arrives without a board reset. No
formatting or card writes are needed for the read-only check. Verify the storage
commands first using an existing file and `storage unmount`; only test mutations on a
disposable file after backing up the card. Follow the
[storage lifecycle board checks](firmware-testing.md#storage-lifecycle-board-checks) for startup mounting,
automatic USB handoff/remount, busy rejection, and failure cases. Do not reset while a filesystem command is in progress.
