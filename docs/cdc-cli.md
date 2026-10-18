# USB CDC CLI

USB CDC ACM is the interactive command interface. UART remains the independent
debug-log transport. Normal storage commands do not change MSC ownership;
explicit confirmed fault recovery is the exception described below. The periodic
CDC greeting has been removed.

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
| `sd status` | Show card presence, initialization, transport, capacity, clock, mounted state, and Pico/USB ownership. |
| `sd info` | Read-only diagnostic snapshot: CID identity, OCR/CCS, RCA, CSD/capacity, SCR/version/widths, requested/actual initialization and transfer clocks, timing, detailed error/phase, init attempts/retries, bounded High-Speed fallbacks and persistent CRC/timeout/PIO stall counters. Native details remain available after failure; SPI reports native details unavailable. |
| `sd profile [status, on, off, reset]` | Show or control optional SD-card phase profiling; changes require Pico ownership. Counters are cumulative and not a total-latency breakdown. Recovery timing/calls and attempts display `reserved` (unmetered for SPI/native); labels and field order are retained. |
| `sd diagnostics [status, clear]` | Show the retained last facade failure, transport initialization stage, owner, and event-counter snapshot. Recovery attempts display `recoveries=reserved`, not a measured zero. Clearing requires Pico ownership. |
| `sd recover --confirm` | Request explicit faulted-MSC detach and worker reinitialization without mounting; not a clean eject, sync, or repair. |
| `fs mount` | Initialize the card and mount the existing FAT/exFAT volume; never format. |
| `fs unmount` | Unmount after preceding commands finish, allowing MSC handoff. |
| `fs ls [path]` | List names, sizes, and directory flags; default path is `/`. |
| `fs stat <path>` | Show size, attributes, and filesystem timestamps. |
| `fs cat <path> [limit]` | Read at most 1024 bytes by default; limit accepts 1..4096 bytes. |
| `fs df` | Show total and free filesystem bytes; scanning free space can take time. |
| `fs mkdir <path>` | Create one directory; parents must exist. |
| `fs write <path> <text>` | Create a new file; fail if it already exists. No newline is added. |
| `fs append <path> <text>` | Append text, creating the file if absent. No newline is added. |
| `fs mv <source> <destination>` | Rename a file/directory without overwriting an existing destination. |
| `fs rm <path> --confirm` | Delete one file with explicit confirmation; directories are refused. |

In NOR builds, the compatibility commands `sd status` and `sd diagnostics` describe the selected NOR media rather
than an SD card. See [NOR diagnostics and recovery](nor-flash.md#build-selected-fatfs-and-usb-msc) for fields and
limitations. Recovery completion or failure is reported through the UART logger; neither outcome mounts a volume.

Storage commands require absolute ASCII paths on volume 0, with no drive
prefixes, wildcards, backslashes, or `.`/`..` segments. Paths are limited to 100
bytes and must fit the CLI's command buffer. Double-quote paths or text containing
spaces. There is no current-directory state, recursive deletion, overwrite, or
format command. File content is displayed in chunks of up to 48 bytes, with
non-printable bytes and backslashes escaped as `\xHH` to protect the terminal.
Long directory names are clipped in listings.

`sd status` and `sd info` are available while USB owns the card; neither sends
commands to the card. Detailed errors and counters do not require `sd profile on`.
The retry count is explicit initialization requests after the first, not runtime
transfer replay. See the [native startup policy](sdcard-design.md#initialization-workflow).
Every `fs` command is refused
until the host safely ejects and disables MSC. The storage worker automatically
attempts to mount detected cards at boot, after insertion, and after MSC returns
ownership. Failed attempts are retried five seconds after each attempt finishes.
Enabling MSC from the dashboard automatically queues `fs` volume unregistration on the sole storage worker;
manual `fs unmount` is optional. A successful unmount pauses
automatic mounting until the next insertion or return from MSC. `fs mount`
remains available for a manual retry.

When board support provides card detect, the storage worker debounces presence
and retries card initialization and FatFs mounting after insertion. Removing a
card while its filesystem is mounted unmounts and invalidates the old volume.
It never formats or repairs a replacement card; unsupported or damaged
filesystems can still fail to mount. Removing media during a write can corrupt
the filesystem; unmount or safely eject before removal. Eject and disable
MSC before swapping cards. If a card is removed while MSC owns it, safely eject
the device and disable MSC after removal so firmware can resume polling.

Only one storage command can be outstanding. The CLI acknowledges that it was
queued; this is not a success report. Wait for its result before the next storage
command. FatFs results include their symbolic name and numeric code. A short
write or sync/close failure is reported; a new or partially modified file may
remain. Commands do not roll back writes. Only explicit fault recovery changes
MSC ownership.

## Faulted MSC Recovery

Use normal host eject followed by HID MSC disable whenever possible. If an I/O
failure invalidates a still-connected card, eject/sync and normal disable may
all fail. For that faulted state only:

1. Stop host access to the drive and collect `sd diagnostics` before recovery.
   Unsaved host data and partially accepted writes may already be lost.
2. Enter exactly `sd recover --confirm` in the CDC CLI. This is a request, not
   a successful clean eject or a promise that initialization will succeed.
3. The request is rejected if MSC is disabled, the card is still initialized,
   removal is prevented by the host, storage work is busy, or the recovery
   request cannot be queued. The existing HID MSC status exposes pending and
   rejected requests; rejected requests do not detach the device.
4. Once accepted, USB detaches before returning card ownership to firmware.
   CDC/HID reconnect without MSC; reopen the same board if necessary.
   The worker attempts card initialization only. It does not retry the failed
   payload, report a successful SCSI sync/eject, mount, format, or repair.
5. After reconnect, use `sd status` to check initialization and confirm
   `mounted=0`; use `sd diagnostics` to inspect retained failures. Successful
   initialization does not erase the original I/O diagnostic. A new failed
   initialization replaces it with the latest failure.
6. Inspect/repair the filesystem with appropriate host tools before deliberately
   running `fs mount`. An initialized card can be handed to MSC again for
   inspection using normal enable/eject/disable. Automatic mounting remains
   suppressed even across reinsertion and subsequent MSC returns until a
   successful explicit `fs mount`. This suppression is not persistent over
   MCU reset or loss of power.

Reinitialization retains checked cleanup of unresolved native streams; it can
fail and leave the card uninitialized. Recovery does not abandon a stream on a
present card or fabricate durability guarantees.

Closing CDC discards the session's partial command, output, and history. Reopening
starts a fresh prompt. A disconnected session stops further listing/cat output,
but accepted mount/unmount and file changes finish safely, with files closed.
The filesystem remains mounted across terminal sessions until explicitly unmounted.
USB re-enumeration likewise resets the CLI session, but a pending storage command
prevents MSC handoff until it is complete and its replies are drained.

Read-only bring-up sequence:

```text
sd status
fs mount
fs ls /
fs stat /existing.txt
fs cat /existing.txt 256
fs df
fs unmount
```

To exercise changes, use a disposable new path and wait for each result:

```text
fs mount
fs mkdir /cli-test
fs write /cli-test/new.txt "hello world"
fs append /cli-test/new.txt " more text"
fs mv /cli-test/new.txt /cli-test/renamed.txt
fs rm /cli-test/renamed.txt --confirm
fs unmount
```

These modification commands change the card. The example leaves the empty
`/cli-test` directory in place because directory removal is deliberately unavailable.

FatFs currently has no RTC integration: newly modified files receive the fixed
timestamp `2026-01-01 00:00:00`. `fs stat` reports the stored timestamp, not the
board's current time. See
[the hardware validation record](../firmware/src/drivers/storage/README.md#sd-card-breakout)
for the tested SPI CLI workflow and remaining qualification limits.

## Integration

[Application CLI](../firmware/src/app/cli/cli.c) owns the static Embedded
CLI instance and command bindings. It has no TinyUSB, board, or FreeRTOS
dependency. Its output callback and input/process API are documented in
[cli.h](../firmware/src/app/cli/cli.h). Add session bindings using
`pf_cli_add_binding()` after initialization; argument-tokenization helpers
come from the upstream framework. Callbacks execute in the USB task, so keep
them bounded and non-blocking; do not run long operations there.

[The USB task](../firmware/src/app/usb/usb_task.c) is the sole owner of CDC and the CLI.
The CLI uses a 4 KiB aligned static arena, 64-byte RX buffer, 128-byte command
buffer, 256-byte history buffer, and eight application-binding slots. The CDC
adapter has a 2 KiB output ring. It processes at most 32 received characters
per task iteration and pauses input when less than 1 KiB of output space remains.
It never waits for the host to consume output. Each process call must emit at
most 1 KiB; adding commands requires preserving that output budget. Exceeding the
ring fails explicitly rather than silently dropping output. HID/MSC continue to
be serviced by the same task.

[The static storage worker](../firmware/src/app/storage/storage_worker.c)
is the sole FatFs caller for CLI operations. It uses an 8 KiB stack, a one-request
queue, and eight 256-byte reply slots. [The command dispatcher](../firmware/src/app/storage/storage_commands.c)
has no RTOS or USB dependency. The worker sends bounded lines through the queue;
only the USB task calls CLI print APIs. Replies carry a session ID so old output
does not leak into a newly opened terminal. Backpressure suspends the worker,
not the USB task. Disconnect cancellation releases read-only output backpressure;
accepted mutations are not abandoned midway. MSC transitions are rejected while
the worker is busy and enabling is rejected while FatFs is mounted. New application
filesystem clients must also be routed through this worker, not a second task.

## Validation

Run the hardware-independent middleware tests:

```sh
tools/validation/setup-ceedling-testing.sh test:test_cli
tools/validation/setup-ceedling-testing.sh test:test_storage_commands test:test_storage_commands_nor
```

They cover prompt/help, commands, invalid arguments, unknown commands, session
reset, completion/history, asynchronous binding registration, and the output
reserve. Mocked FatFs tests cover commands, ownership/mount guards, safe paths,
create-new/append modes, close/sync and partial-write errors, bounded cat output,
directory cleanup, confirmed deletion, and cancellation. They do not verify actual
card I/O, FreeRTOS queue scheduling, USB CDC enumeration, or host backpressure.

For a board check, safely eject and disable any active MSC drive before flashing.
Open CDC and run `help`, `version`, and `echo CDC test`. Close/reopen to verify a
fresh prompt, and check that HID reports continue during commands. Stop reading
CDC briefly, resume, and confirm output arrives without a board reset. No
formatting or card writes are needed for the read-only check. Verify the storage
commands first using an existing file and `fs unmount`; only test mutations on a
disposable file after backing up the card. Verify that MSC enable is rejected
while mounted or busy and allowed after unmount; after host eject/disable, confirm
that `fs mount` works again. Do not reset while a filesystem command is in progress.