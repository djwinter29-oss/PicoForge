# Qualification Gaps

PicoForge's existing feature set is maintained, with hardware qualification still incomplete.
A successful build or short test does not qualify sustained SD-card
reliability, USB mass-storage compatibility, or power-loss recovery. Back up important card data before use and
follow the [getting-started safety precautions](../guides/getting-started.md#before-you-connect).

## Areas Not Fully Qualified

- **Sustained SD-card reliability:** existing measurements apply to specific firmware, hardware, cards, and test
  durations. They do not establish reliability across other configurations or sustained workloads.
- **Physical USB mass-storage compatibility:** host tests and source contracts do not establish end-to-end USB
  behavior across host operating systems, including eject, disconnect, reset, and error recovery.
- **Power-loss recovery:** existing validation does not establish filesystem integrity or data durability after
  interrupted writes or unexpected power loss.
- **W-board radios:** physical Wi-Fi association/reconnect, BLE access/control, and coexistence with USB and storage
  remain unqualified. See the
  [wireless and BLE board checks](../development/firmware-testing.md#wireless-and-ble-board-checks).
- **Direct dashboard hardware lifecycle:** host/browser mocks cover draft ownership, cancellation, expiry and
  recovery, but do not qualify USB/BLE reconnect, physical staging cleanup or forced saves on Pico W/Pico 2 W.
  Follow the [Direct board checks](../development/firmware-testing.md#direct-dashboard-board-checks) and record
  each board/OS/browser combination separately.
- **Python host timing and lifecycle:** the 50 ms HID polling interval, real report freshness, CPU cost under multiple
  monitors, USB detach/reconnect, MSC ownership handoff, completed-save/reboot gating, duplicate-identity recovery, and
  shutdown during wireless operations or TCP authentication have not yet been qualified on physical boards.
  Hardware-free tests cover these lifecycle gates but do not close this gap. Follow the
  [Python host hardware procedure](../development/host-tools.md#qualify-the-python-host-with-hardware).
- **.NET host timing and lifecycle:** the 100 ms HID polling interval, report freshness, multi-board CPU cost, USB
  detach/reconnect, forced-save recovery, and MSC ownership handoff have not yet been qualified on physical boards.
  Follow the [.NET host hardware procedure](../development/host-tools.md#qualify-the-net-host-with-hardware).

These are evidence gaps, not a claim that every listed scenario fails.

## Evidence and Follow-up

- [Test and review records](records/README.md): dated measurements and reviews with configuration-specific scope.
- [SD remediation matrix](sd-review.md): detailed review dispositions and outstanding measurement needs.
- [Firmware development and testing](../development/firmware-testing.md): validation procedures and hardware limits.

Keep detailed findings and test results in those authoritative documents rather than duplicating them here.
