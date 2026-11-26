# SD-Card Storage

[Documentation](../README.md) / [User guides](README.md)

Use an existing FAT/exFAT card with data you have backed up. Follow [hardware setup](hardware.md#memory-and-storage-connections)
before connecting a breakout. Automatic mounting never formats the card.

## Read Files on the Board

Firmware mounts the card at startup, after insertion, and after normal USB return. Opening a dashboard, connecting
CDC, or starting a benchmark is not a mount request. Wait for `mounted`, not merely `ready`, before using files.

In the [CDC console](console.md#terminal), check status and read an existing file:

```text
storage get status
fs ls /
fs cat /example.txt
```

Replace `/example.txt` with a file already on your card. For path rules, modification commands, formatting, and
manual mount examples, see [console storage commands](console.md#storage-and-filesystem-commands).

## Share the Card with Your Computer

Firmware and the computer cannot use the filesystem at the same time.

1. Finish firmware file operations. Manual `storage unmount` is optional; enabling USB mass storage queues an unmount.
2. Enable USB mass storage (MSC) in the dashboard. The medium becomes available without disconnecting HID or CDC.
3. Use the card from your computer. Firmware file commands and benchmarks remain unavailable while USB owns it.
4. Stop host file activity and safely eject through the operating system. For normal return, use
   `storage set usb off` in the console and wait for ownership to return. The dashboard's **Return to Pico** action
   instead requests confirmed access revocation/remount; it is not an OS eject. Read the
   [force-return risks](../reference/usb-hid.md#msc-force-return-safety) before using it.
5. Wait for fresh mounted status before using files or starting a benchmark. Normal return remounts automatically.
   A failed forced-return mount requires inspection and deliberate `storage mount`.

The [USB HID reference](../reference/usb-hid.md#msc-force-return-safety) owns exact return commands and safeguards;
the [storage lifecycle](../reference/architecture.md#storage-ownership-lifecycle) owns the state transitions.

## Remove or Replace a Card

- Never remove a card during writes or a benchmark. Unmount or safely eject first.
- While firmware owns the card, `storage unmount` pauses automatic mounting until insertion or normal MSC return.
  Use `storage mount` for a deliberate manual retry.
- Where card detect is wired, the worker debounces insertion and retries initialization/mounting. Removing a mounted
  card invalidates the old volume; inserting a replacement never formats or repairs it.
- If removal happens while USB owns the card, safely eject the device and disable MSC after removal so firmware can
  resume polling.

## If Mounting or I/O Fails

`storage get status` remains available while USB owns the card and does not issue card commands. `busy` means a
storage operation held the media when the snapshot was taken. Card identity and backend diagnostics are in the dashboard.

Normal mount failures retry five seconds after each attempt finishes unless recovery has blocked retries.
Inspect the retained error before manually retrying; successful initialization alone does not establish a healthy
filesystem. A still-connected but faulted USB-owned card needs the
[faulted MSC recovery procedure](console.md#faulted-msc-recovery), not repeated file writes or forced retries.

For throughput testing, follow the [benchmark guide](sdcard-benchmark.md). It writes a temporary file and does not
repair or qualify the card. See [known limits](../evidence/README.md) before relying on sustained or power-loss behavior.
