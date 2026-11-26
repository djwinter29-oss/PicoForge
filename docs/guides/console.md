# Console: USB and Wi-Fi

[Documentation](../README.md) / [User guides](README.md)

This guide owns terminal usage, CLI commands, and CDC/storage behavior. For interface roles and the
authoritative HID contract, see the [USB HID report reference](../reference/usb-hid.md).

USB CDC ACM is the interactive command interface. UART remains the independent debug-log transport. File operations
do not change MSC ownership; use explicit storage commands or the dashboard for handoff.

On this page: [terminal](#terminal) / [commands](#commands) / [settings](#board-configuration) /
[files](#storage-and-filesystem-commands) / [TCP](#wi-fi-tcp-terminal) / [fault recovery](#faulted-msc-recovery).

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
The editor accepts at most 126 command-line bytes. Editor or receive-buffer overflow rejects the command through
the next CR/LF and prints `ERR input overflow`; its accepted prefix is never executed. Backspace, history, and
completion do not restore an overflowed command. Re-enter it after the error or reopen the session.

## Commands

`help` groups commands by name, with one indented subcommand per line. Prefix each subcommand with its group name:
`get clock` under `board` means `board get clock`. Use `help <command>` or `<command> --help` to show only that group's
usage. Angle brackets mark required arguments, square brackets mark optional arguments, and `|` separates choices.
The web console uses the same eight-column tab stops as a normal terminal to preserve help indentation.

Mutation replies use consistent prefixes: `OK` means completed, `QUEUED` means accepted but incomplete, `ERR` means
rejected/failed, and `WARN` means partial output or an uncertain/partial result. Status and data replies have no
prefix. A queued USB drive or benchmark response acknowledges admission, not completion.

| Command | Behavior |
| --- | --- |
| `help` | List commands; `help <command>` displays command help. |
| `board get <clock\|led\|version\|uptime>` | Read the actual system clock in Hz, LED state, firmware version, or uptime. |
| `board set led <on\|off>` | Set the board-owned LED through the same platform service as the dashboard. |
| `board reboot` | Schedule a reboot; refused while storage is busy, USB-owned, or changing ownership. |
| `config get [clock\|storage\|wifi\|bluetooth]` | Read saved next-boot targets, not actual running hardware values. |
| `config set <key=value>[, key=value...]` | Validate every assignment, save all targets together, then reboot. |
| `radio status` | Read live Wi-Fi state and IP address, and Bluetooth state. |
| `storage get [status]` | Show the backend-neutral storage state (`not_configured`, `busy`, `absent`, `uninitialized`, `ready`, `mounted`, `usb`), Pico/USB ownership, medium, bus, presence, bus clock, capacity, and the last retained failure as `<operation>:<result>`. |
| `storage get usb` | Read USB drive enabled, ejected, pending and rejected ownership flags. |
| `storage set usb <on\|off>` | Queue normal USB drive ownership transfer; check `storage get usb` for completion or rejection. |
| `storage get benchmark` | Read retained benchmark progress, timings, mismatches, run/admission errors and cleanup flags. |
| `storage benchmark <MiB>` | Queue a 1..1024 MiB file benchmark on the storage worker; preflight can still reject it. |
| `storage benchmark cancel` | Request cancellation; wait for cleanup to finish before other storage work. |
| `storage recover --confirm` | Revoke access to faulted MSC media and request worker reinitialization without mounting; not a clean eject, sync, or repair. |
| `storage mount` | Maintenance only: manually initialize/mount after inspection or explicit unmount; normally startup/return mounting is automatic. Never format. |
| `storage unmount` | Maintenance only: suppress automatic mounting until insertion or normal MSC return. USB enable also unmounts automatically. |
| `storage format --confirm` | Destroy files on the selected logical device, create FAT12/16/32 (automatically selected by FatFs) without a partition table, then mount. Requires Pico ownership and an unmounted volume. |
| `fs ls [path]` | List names, sizes, and directory flags; default path is `/`. |
| `fs stat <path>` | Show size, attributes, and filesystem timestamps. |
| `fs cat <path> [limit]` | Read at most 1024 bytes by default; limit accepts 1..4096 bytes. |
| `fs df` | Show total and free filesystem bytes; scanning free space can take time. |
| `fs mkdir <path>` | Create one directory; parents must exist. |
| `fs write <path> <text>` | Create a new file; fail if it already exists. No newline is added. |
| `fs append <path> <text>` | Append text, creating the file if absent. No newline is added. |
| `fs mv <source> <destination>` | Rename a file/directory without overwriting an existing destination. |
| `fs rm <path> --confirm` | Delete one file with explicit confirmation; directories are refused. |

Recovery completion or failure is reported through the UART logger; neither outcome mounts a volume.

`storage get benchmark` reports lifecycle names and numeric errors from the
[benchmark API](../../firmware/src/app/storage/file_benchmark.h); `flags=1` means the test file remains after cleanup.
Flags can be combined; bit 1 records that the caller's volume was mounted. A queued start can still fail worker
preflight; inspect `request_error` as well as the run's `error`. Follow the
[benchmark procedure](sdcard-benchmark.md) before starting a write test.

## Board configuration

```text
PicoForge> board get clock
clock=150000000 Hz
PicoForge> config get clock
clock=default (next boot)
PicoForge> config set clock=150, storage=high
OK config saved
QUEUED reboot; waiting for CLI output to flush
```

`clock` accepts `default`, `150`, `200`, or `250` MHz. `storage` accepts `default` (25 MHz target) or `high` (50 MHz
target); the backend/card may fall back. On W boards, `wifi` and `bluetooth` accept `enabled` or `disabled`.
These radio master switches preserve existing credentials, matching the HID boot-profile controls. Enabling a switch
does not provision a network or Bluetooth key. To configure or erase credentials, use the USB dashboard's radio
configuration dialogs. Passwords and keys are deliberately not accepted/read back by the CLI: command input is echoed,
retained in history, and the raw TCP connection is unencrypted.

Assignments are comma-separated, may contain whitespace around `=`, and must fit in 99 bytes after `config set `.
Unknown keys, duplicate keys, missing values, empty assignments, and invalid values reject the whole request before
staging. Successful persistence schedules reboot automatically; no separate save step is needed. A failed/unconfirmed
save discards staged targets and does not reboot, but flash may already have changed: inspect `config get` before retrying.
The running clock does not change until reboot. A `default` target means the SDK/build clock, not a fixed board frequency.
150 MHz on RP2040 and 200/250 MHz profiles are not hardware-qualified; use them only with the stability risks understood.
See the [system clock API](../../firmware/src/platform/system.h) for the platform constraints.

Saving/resetting is refused while storage work, MSC ownership or an ownership transition is outstanding. Safely eject
USB storage, run `storage set usb off`, and wait for `storage get usb` to show `usb=disabled pending=0 rejected=0`
before saving. There is no CLI forced save or unsafe MSC return. Radio-disabled firmware rejects radio
commands/settings. Benchmark-disabled firmware reports `ERR benchmark unavailable in this build`.

## Storage and filesystem commands

For automatic mounting, USB handoff, card removal, and mount failures, use the [storage guide](storage.md).
`storage mount` and `storage unmount` are maintenance operations, not routine prerequisites or saved boot settings.

Formatting is destructive and still requires an unmounted, Pico-owned volume. After backing up the card and safely
ejecting/disabling USB storage, run `storage unmount` and wait for completion before `storage format --confirm`.
A successful format mounts the new filesystem automatically. The old `fs mount`, `fs unmount` and `fs format` names
are no longer accepted; `fs` contains only file/directory operations.

File commands require absolute ASCII paths on volume 0, with no drive
prefixes, wildcards, backslashes, or `.`/`..` segments. Paths are limited to 100
bytes and must fit the CLI's command buffer. Double-quote paths or text containing
spaces. There is no current-directory state, recursive deletion, or file overwrite.
Directory listings display up to 200 filename bytes without splitting UTF-8 characters and append
`[name truncated]` when shortened. The marker is not part of the filename.
If any entry is outside the CLI's ASCII/100-byte path limits, one summary `WARN` follows the listing; use host file
tools for those entries. Listing a name does not imply it can be used as a CLI path.
File content is displayed in chunks of up to 48 bytes, with
non-printable bytes and backslashes escaped as `\xHH` to protect the terminal.
Long directory names are clipped in listings.

Every `fs` command is refused while USB owns the card. Wait for `mounted` status before reading/writing;
`ready` means initialized but unmounted.

Only one storage command can be outstanding. The CLI acknowledges that it was
queued; this is not a success report. Wait for its result before the next storage
command. FatFs results include their symbolic name and numeric code. A short
write or sync/close failure is reported; a new or partially modified file may
remain. Commands do not roll back writes or change MSC ownership.

Closing CDC discards the session's partial command, output, and history. Reopening
starts a fresh prompt. A disconnected session stops further listing/cat output,
but accepted mount/unmount and file changes finish safely, with files closed.
The filesystem remains mounted across terminal sessions until explicitly unmounted.
USB re-enumeration likewise resets the CLI session, but a pending storage command
prevents MSC handoff until it is complete and its replies are drained.

For a manual mount/read/unmount check, wait for each result:

```text
storage get status
storage mount
fs ls /
fs stat /existing.txt
fs cat /existing.txt 256
fs df
storage unmount
```

To exercise changes, use a disposable new path and wait for each result:

```text
storage mount
fs mkdir /cli-test
fs write /cli-test/new.txt "hello world"
fs append /cli-test/new.txt " more text"
fs mv /cli-test/new.txt /cli-test/renamed.txt
fs rm /cli-test/renamed.txt --confirm
storage unmount
```

These modification commands change the card. The example leaves the empty
`/cli-test` directory in place because directory removal is deliberately unavailable.

FatFs currently has no RTC integration: newly modified files receive the fixed
timestamp `2026-01-01 00:00:00`. `fs stat` reports the stored timestamp, not the
board's current time. The tested SPI CLI workflow and its limits are recorded
in the [historical SD-card bring-up observations](../evidence/records/sdcard-bringup-observations.md#historical-sd-card-qualification);
they are not current hardware qualification.

## Wi-Fi TCP terminal

Pico W and Pico 2 W boards with Wi-Fi and the saved **TCP CLI enabled** switch on serve the same CLI as a raw TCP
stream on port 2323. Find the board IP in the dashboard's live diagnostics, or use `picoforge-<USB serial>.local`
with an mDNS-capable host resolver on the same multicast-capable LAN.
Save a [Control key](wireless.md#configuration-and-operation) before using the TCP CLI. Dashboard consoles authenticate
without entering the key in the console or sending it to the local bridge: USB clients request a fixed-purpose proof
from the board; Direct over BLE signs with its already-unlocked session key. Keep Direct's matching dashboard connected
while its console connects or reconnects; see [page lifecycle](../../host/direct/README.md#page-lifecycle).
Custom clients must implement [TCP CLI authorization](../development/wireless-integration.md#tcp-cli-authorization)
and answer each one-use challenge within ten seconds.
After authentication, use raw terminal mode with local echo and line editing disabled; the CLI owns both.

TCP is unencrypted. Any authenticated client can run every CLI command, including destructive commands.
Use only a trusted LAN.

In the Python and .NET dashboards, the connected **SSID (IP)** label in Live diagnostics becomes a link when the
firmware reports that the TCP CLI is serving. Clicking it opens a separate web console tab and connects
automatically. Type in the terminal; there is no local echo. Use **Disconnect** to release it or **Connect** to
reconnect. Closing the tab also releases the session; an abandoned tab expires after 15 seconds without a browser
request.
Each Python or .NET dashboard process allows one active console session at a time; the session is bound to the selected
USB board.
Browser requests have bounded deadlines; disconnect aborts pending requests, and input with an uncertain delivery
result is never automatically resent.
The local host relays bounded raw bytes over TCP; browsers use the local bridge because they cannot open raw TCP
sockets. Python and .NET select a target from the connected board's wireless status, not a user-supplied host or port.
Direct provides the console only when hosted locally with its included bridge; its hosted static site does not.

The web console supports the Embedded CLI's cursor movement, character insertion/deletion and saved cursor escapes.
It keeps at most 1000 lines of 512 columns; it is not a general-purpose VT100/color terminal. Input is limited to
1024 bytes per pending batch. Pasting text containing Enter asks for confirmation before executing it.

One CLI session exists at a time. The first transport to connect owns it: a TCP client connecting while a CDC terminal
holds DTR receives `PicoForge CLI busy: a USB CDC session is active.` and is closed; a CDC terminal opened during a
TCP session receives a busy line and its input is discarded. A second TCP client is turned away the same way. Closing
the owning connection ends the session and cancels its pending output, exactly like dropping DTR. TCP keepalive
drops a vanished client after about 50 seconds idle.

Rejected proofs are rate-limited for two seconds. The firmware closes authenticated sessions after 15 minutes without
traffic.

## Faulted MSC Recovery

Use normal host eject followed by HID MSC disable whenever possible. If an I/O
failure invalidates a still-connected card, eject/sync and normal disable may
all fail. For that faulted state only:

1. Stop host access to the drive and record `storage get status` before recovery.
   Unsaved host data and partially accepted writes may already be lost.
2. Enter exactly `storage recover --confirm` in the CDC CLI. This is a request, not
   a successful clean eject or a promise that initialization will succeed.
3. The request is rejected if MSC is disabled, the card is still initialized,
   removal is prevented by the host, storage work is busy, or the recovery
   request cannot be queued. The existing HID MSC status exposes pending and
   rejected requests; rejected requests do not detach the device.
4. Once accepted, MSC access is revoked before returning card ownership to firmware.
   CDC and HID stay connected; MSC reports no medium.
   The worker attempts card initialization only. It does not retry the failed
   payload, report a successful SCSI sync/eject, mount, format, or repair.
5. After the worker finishes, use `storage get status` to check that the state is `ready`, not
   `mounted`, and to inspect the retained `last_error`. Successful
   initialization does not erase the original I/O diagnostic. A new failed
   initialization replaces it with the latest failure.
6. Inspect/repair the filesystem with appropriate host tools before deliberately
   running `storage mount`. An initialized card can be handed to MSC again for
   inspection using normal enable/eject/disable. Automatic mounting remains
   suppressed even across reinsertion and subsequent MSC returns until a
   successful explicit `storage mount`. This suppression is not persistent over
   MCU reset or loss of power.

Reinitialization retains checked cleanup of unresolved native streams; it can
fail and leave the card uninitialized. Recovery does not abandon a stream on a
present card or fabricate durability guarantees.

## Stalled output

USB tracks successful transfers from the CLI ring to TinyUSB. After five seconds with buffered output and no
progress, it cancels reply production for that DTR session and drains the worker queue, including completion,
even while the host is not reading. The session accepts no further input until DTR closes and reopens. A stalled TCP session is closed instead, because
its client cannot read the notice.
Buffered output keeps its order; when space becomes available an explicit truncation notice follows it.
Disconnect clears buffered output and drains completion as before. USB continues its paced service loop.

This is a bounded-memory protocol trade-off: a slow host can receive incomplete command output. Cancellation
stops display producers, not completed filesystem mutations; file close/cleanup still precedes completion.
Listing and cat handlers attempt an explicit interrupted-output notice before the final FatFs status when their
reply callback stops accepting data. A disconnected client may receive neither; `OK FatFs: OK` describes filesystem
operations, not proof of complete output delivery.
A missing success reply does not mean a write or format failed. Inspect state before retrying a mutation.
The timeout measures firmware buffer progress, not proof of host application consumption.

## Integration Reference

See [console integration](../development/console-integration.md) for dependency setup, command registration,
transport/session internals, and software validation.
