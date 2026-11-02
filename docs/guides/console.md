# Console: USB and Wi-Fi

This guide owns terminal usage, CLI commands, and CDC/storage behavior. For interface roles and the
authoritative HID contract, see the [USB HID report reference](../reference/usb-hid.md).

USB CDC ACM is the interactive command interface. UART remains the independent
debug-log transport. Normal storage commands do not change MSC ownership;
explicit confirmed fault recovery is the exception described below. The periodic
CDC greeting has been removed.

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

`help` groups commands by name, with one indented subcommand per line. Prefix each subcommand with its group name:
`get clock` under `board` means `board get clock`. Use `help <command>` or `<command> --help` to show only that group's
usage. Angle brackets mark required arguments, square brackets mark optional arguments, and `|` separates choices.
The web console uses the same eight-column tab stops as a normal terminal to preserve help indentation.

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

### Board configuration

Every group follows PicoPWM's `group verb parameters` convention: `get` reads, `set` changes a value, and other verbs
are actions such as `board reboot` or `storage benchmark 64`. There are five groups: `board`, `config`, `storage`, `fs`
and `radio`; use `help <group>` for its syntax. `storage` owns device management (status, USB drive ownership,
formatting, benchmark, recovery and maintenance mounting), while `fs` works with files on the mounted volume.
Use `board get version` for the firmware version.
Mutation replies use consistent prefixes: `OK` means completed, `QUEUED` means accepted but incomplete, `ERR` means
rejected/failed, and `WARN` means partial output or an uncertain/partial result. Status and data replies have no
prefix. A queued USB drive or benchmark response acknowledges admission, not completion.

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

`storage get benchmark` reports lifecycle names and numeric errors from the
[benchmark API](../../firmware/src/app/storage/file_benchmark.h); `flags=1` means the test file remains after cleanup.
Flags can be combined; bit 1 records that the caller's volume was mounted. `storage benchmark <MiB>` uses the existing
mounted volume, never formats, and refuses an existing `PFTEST.BIN`. A queued start can still fail worker preflight;
inspect `request_error` as well as the run's `error`. Cancellation is a request, not a completed cleanup.

### Storage and filesystem commands

Mounting/unmounting is automatic during normal startup and USB drive handoff. `storage mount` and `storage unmount`
remain maintenance commands for manual retry, safe removal, formatting and post-recovery inspection; they are not
routine prerequisites. These are live operations, not saved boot settings under `config`.

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

`storage get status` is available while USB owns the card and never sends commands to the card; `busy` means another
storage operation held the media at the time of the snapshot. Card identity and backend details are exposed only
through the HID dashboard.
Every `fs` command is refused
until the host safely ejects and disables MSC. Follow the authoritative
[default-mounted storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle):
mounting happens at startup, after insertion, and after normal MSC return, not on a self-test click or CDC connection.
Wait for `mounted` status before reading/writing; `ready` means initialized but unmounted.
Normal mount failures are retried five seconds after each attempt finishes, except when recovery blocks retries.
Enabling MSC from the dashboard automatically queues `fs` volume unregistration on the sole storage worker;
manual `storage unmount` is optional. A successful unmount pauses
automatic mounting until the next insertion or return from MSC. `storage mount`
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

### Wi-Fi TCP terminal

Pico W and Pico 2 W boards with Wi-Fi and the saved **TCP CLI enabled** switch on serve the same CLI as a raw TCP
stream on port 2323. Find the board IP in the dashboard's live diagnostics, or use `picoforge-<USB serial>.local`
with an mDNS-capable host resolver on the same multicast-capable LAN.
In PuTTY choose connection type **Raw**, host `<board IP>` (or the mDNS name), port `2323`, and
set **Terminal > Local echo** and **Local line editing** to **Force off**; the CLI echoes and edits input itself.
PuTTY's default **Auto** enables both for Raw connections, which shows each command twice and sends it only on Enter,
so Tab completion and history keys do not work. Save the settings as a named session. On Linux,
`nc <board IP> 2323` also works after `stty raw -echo` in the local terminal.

In the Python and .NET dashboards, the connected **SSID (IP)** label in Live diagnostics becomes a link when the
firmware reports that the TCP CLI is serving. Clicking it opens a separate web console tab and connects
automatically. Type in the terminal; there is no local echo. Use **Disconnect** to release it or **Connect** to
reconnect. Closing the tab also releases the session; an abandoned tab expires after 15 seconds without a browser request.
The local host relays bounded raw bytes over TCP, using same-origin HTTP polling between browser and host; browsers
cannot open raw TCP sockets themselves. Targets come only from the selected USB board's wireless status, not a
user-supplied host or port. Once connected, the console does not require the USB connection to remain open.
The Direct dashboard has no local bridge, so use PuTTY there. Flash the updated firmware and restart the local
dashboard to enable the link; older firmware that does not report listener state retains the plain-text label.

The web console supports the Embedded CLI's cursor movement, character insertion/deletion and saved cursor escapes.
It keeps at most 1000 lines of 512 columns; it is not a general-purpose VT100/color terminal. Input is limited to
1024 bytes per pending batch. Pasting text containing Enter asks for confirmation before executing it.

One CLI session exists at a time. The first transport to connect owns it: a TCP client connecting while a CDC terminal
holds DTR receives `PicoForge CLI busy: a USB CDC session is active.` and is closed; a CDC terminal opened during a
TCP session receives a busy line and its input is discarded. A second TCP client is turned away the same way. Closing
the owning connection ends the session and cancels its pending output, exactly like dropping DTR. TCP keepalive
drops a vanished client after about 50 seconds idle.

TCP authentication is optional and uses the board's saved BLE control key. When enabled, clients must answer a
one-use challenge within ten seconds; the Python and .NET web consoles ask for the key and perform the proof. Raw
PuTTY/`nc` sessions need a client that implements the proof described in the
[wireless reference](../reference/wireless.md#ble-contract-and-authorization). Rejected proofs are rate-limited for
two seconds. The firmware closes an otherwise authenticated session after 15 minutes without traffic; keepalive
still detects a vanished peer after about 50 seconds. Authentication does not encrypt TCP traffic, and anyone who
can connect can run every CLI command, including `--confirm` destructive ones. Use only a trusted LAN.
Firmware logs include the peer IP for connections, successful/rejected proofs, cooldown refusals, challenge failures,
and authentication or idle timeouts. Keys, proofs and command contents are not included in these connection logs.

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

Closing CDC discards the session's partial command, output, and history. Reopening
starts a fresh prompt. A disconnected session stops further listing/cat output,
but accepted mount/unmount and file changes finish safely, with files closed.
The filesystem remains mounted across terminal sessions until explicitly unmounted.
USB re-enumeration likewise resets the CLI session, but a pending storage command
prevents MSC handoff until it is complete and its replies are drained.

Read-only bring-up sequence:

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
board's current time. See
[the hardware validation record](hardware.md#sd-card-breakout)
for the tested SPI CLI workflow and remaining qualification limits.

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
