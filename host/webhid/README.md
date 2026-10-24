# PicoForge WebHID

Serve the WebHID directory directly so its `index.html` is the site root and the browser gets a secure localhost origin:

```sh
python3 -m http.server 8000 --directory host/webhid
```

Open `http://localhost:8000/` in a WebHID-capable browser and grant access to the vendor HID interface.
The page owns its HTML, CSS, view logic, and favicon; it does not use USB serial or a host server API.

## Protocol compatibility

This standalone client targets layout version 1 of the authoritative
[HID Report Reference](../../docs/design/usb/hid-report-reference.md), which remains a draft firmware design.
It requires Feature 1 identity (26 payload bytes), Feature 2 board configuration (17), Feature 20 control/result
(9), and Input 3 telemetry (22), 10 storage (34), and optionally 11 self-test (32, capability bit 5).
Firmware or Python/.NET/shared dashboard integrations still using the legacy contract are not compatible with
this client update. This standalone change does not update firmware, descriptors, or those integrations.
Mandatory identity/configuration failures are explicit connection errors, not legacy fallbacks.

WebHID supplies payload bytes without the report ID. Exact lengths are required; this adapter does not guess framing
from byte values or discard padding. Telemetry expires at 750 ms, storage and self-test at 1500 ms.
Stale measurements are removed, freshness is shown per report, and disconnect clears all connection caches.
Firmware RAM reserved / total is boot-stable build accounting, not live heap or stack usage.

## Controls

All operations serialize Feature 20 SET/GET transactions, matching both transaction ID and opcode.
An acceptance result does not prove storage ownership changed or a self-test finished; the dashboard waits for
fresh corresponding Input status. A timeout or disconnect never replays a command; uncertain results block further
writes. Reconnecting reads the outstanding result without resubmitting it. Do not run competing HID writers.

**Save settings** verifies the result and rereads Feature 2. Profiles apply at next boot; saving does not reboot.
**Reboot** is separate, capability-gated, and explicitly confirmed. Standard/High-Speed is a stored request;
Storage shows the actual selected timing and effective bus clock.

USB ownership transitions briefly re-enumerate; the dashboard reopens the same authorized device by USB serial
and repeats mandatory Feature reads. **Return to Pico** is explicitly confirmed force return, not an OS eject.
Stop host file activity and safely eject whenever possible first. See the
[force-return risks](../../docs/design/usb/hid-report-reference.md#msc-force-return-safety).
The card is not automatically formatted or repaired, and reconnect does not prove successful remount.

The optional self-test requires fresh, known self-test status and an already-mounted filesystem.
Start authorizes temporary `/PFTEST.BIN` writes; an existing file is rejected, never replaced.

## Checks

```sh
node --test host/webhid/tests/*.test.mjs
```

These are host-only protocol, transaction, freshness, and UI checks. Physical WebHID/USB behavior still requires
compatible firmware and RP2040/RP2350 hardware qualification.