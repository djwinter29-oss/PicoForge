# Wireless Integration and Protocol

This reference owns radio execution, credential persistence, USB provisioning reports, and BLE authorization.
For user setup and restrictions, see [the wireless guide](../guides/wireless.md).

## Execution and storage safety

The application groups radio-specific code under `firmware/src/app/wireless`: `wifi/` contains `wifi.c/.h` for station
connection and reconnects, plus `tcp_cli.c/.h` for the raw TCP console. `bluetooth/` contains `bluetooth.c/.h` for BLE
GATT and authorization state, `bluetooth_auth.c/.h` for proof verification, and `bluetooth_profile.gatt` for the service
definition. The common `wireless.c/.h` remains at the wireless root and owns shared SDK initialization/polling,
combined status and atomic USB provisioning. Radio-specific modules keep their boot-applied secrets private.

SDK poll-mode wireless work runs on the existing USB owner task. It creates no additional FreeRTOS tasks, preserves
the static-allocation policy, and never invokes FatFs from a radio callback. On W images that owner task is pinned to
its startup core before initializing the SDK at boot: poll-context checks and CYW43 IRQ ownership require it. The same
task drives the CYW43 LED. Non-wireless builds keep the existing all-core task affinity. BLE controls reuse the existing LED
and benchmark admission helpers. USB and BLE retain separate command-result slots, with the original benchmark
admission tied to its source. Storage ownership remains exclusive.

SDK BTstack's default flash-TLV initializer is interposed to select its nonpersistent TLV implementation before
any bank access. Its default Pico W flash banks otherwise overlap the settings sector. No OS bonds are persisted;
application proof authorization does not rely on Bluetooth pairing. Only the existing boot configuration service
writes flash; persistent Bluetooth bonding is not supported.

The unchanged 16-byte version-1 clock/speed record is followed by a 144-byte wireless extension in the same page:
little-endian magic `0x4c574650`, version `1`, size `144`, a reserved byte/SSID[33]/password[64]/key[32], two
reserved zero bytes, and CRC-32 over its first 140 bytes. The first byte was a pair of provisioning flags in earlier
firmware; it is ignored when loading those records and written as zero on the next save. Old records retain
default-enabled master switches but have no provisioned credentials: Wi-Fi stays off and BLE can advertise view-only.
Every save sets the profile record's wireless-required byte, so a bad or torn extension loads complete defaults rather
than new profiles without credentials; older records (byte `0`) without an extension keep their profiles. Profile saves
preserve the extension. A Feature 31 update stages the clock, SD profile, radio switches and all credentials and saves
once; any rejection or failure discards the staged values, so no partial change survives.
Downgrading to firmware that predates wireless support can erase the extension on its next settings save;
reprovision wireless credentials after upgrading again.
Flash remains **unencrypted**: physical/debug access can recover credentials. Erasing settings clears them.
Passwords and keys are never included in status, diagnostic logs or readback reports.

## USB contract

Identity capability bit 7 denotes wireless provisioning. The radio master switches use Report 2 `board_flags`
bits 3-4 and opcode 6 argument byte 3; see [USB HID report reference](usb-hid.md). Feature 30
reports the saved radio switches; its separate booleans indicate whether credentials are configured.

- Feature **30 GET**, 48 payload bytes: version at 0, saved radio-switch flags at 1 (Wi-Fi bit 0, BLE bit 1), last
  provisioning status at 2 (existing command-status values), transaction at 3, Wi-Fi state at 4
  (disabled/connecting/connected/failed = 0/1/2/3), IPv4 bytes at 5-8, NUL-terminated SSID at 9-41,
  password/key-configured booleans at 42/43, error at 44 (none/init/join/timeout = 0/1/2/3), BLE link at 45
  (off/advertising/connected = 0/1/2; off until the controller is working), TCP CLI listener-started boolean
  at 46 (zero on older firmware), zero at 47. Older hosts that reject reserved byte 46 must be updated
  alongside firmware that starts the TCP listener.
- Feature **31 SET**, 136 payload bytes: version `2` at 0, nonzero transaction at 1, radio switches at 2 (Wi-Fi
  bit 0, BLE bit 1), flags at 3 (preserve password bit 0, preserve key bit 1, use successfully tested Wi-Fi
  credentials bit 2), SSID at 4-36, password at 37-100, key at 101-132, little-endian clock MHz at 133-134
  (opcode 6 values) and SD profile at 135. A cleared preserve flag with an empty password or all-zero key erases
  that credential. Other versions or radio flags inconsistent with the settings are rejected.
  There is **no Feature 31 readback**; hosts confirm the board fields through Report 2.
- Feature **34 SET**, 101 payload bytes: version `1` at 0, operation at 1 (scan `1`, select page `2`, test join
  `3`, clear staged test `4`), nonzero transaction at 2, page at 3, SSID/password byte lengths at 4/5, SSID bytes
  at 6-37 and temporary password at 38-100. Unused fields are zero. Password data is never returned.
- Feature **35 GET**, 220 payload bytes: version, transaction, operation state, page/detail, total scan count and
  Wi-Fi state at offsets 0-5. Byte 3 is the selected page (0-2) in scan states and the test detail in states 4-6:
  none `0`, searching `1`, checking password `2`, waiting for DHCP `3`, network not found `4`, password rejected
  `5`, no DHCP address `6`, timed out `7`. Six entries of 35 bytes each start at 6: SSID length, a 32-byte SSID,
  signed RSSI and secured flag. Bytes 216-219 contain the IPv4 address only after a successful test join.
  Operation states are idle `0`, scanning `1`, scan complete `2`, scan failed `3`, joining `4`, joined `5`, and
  failed `6`. The scan list is capped at 18 strongest unique SSIDs (three pages).
- Native-host `GET /api/wireless` returns `{wireless, reboot_pending}` with sanitized fields. Confirmed
  `POST /api/wireless` accepts `wifi_enabled`, `ble_enabled` (credential intent), `ssid`, optional `password`,
  optional 64-hex-digit `ble_key`, required `board` (the `/api/settings` clock, profile and radios object) and
  required `confirm_reboot:true`. It returns `{wireless, settings, reboot_pending}` after verifying both reports.
  Omitted secrets are preserved; secrets are never returned.
  Existing board query selection and error handling apply.
- `POST /api/wireless/scan` returns nearby sanitized SSIDs, RSSI and security flags. `POST /api/wireless/test`
  accepts an SSID and temporary password and returns join state/IP without saving credentials.
  `POST /api/wireless/cancel` clears a successful temporary test. Passwords are accepted only in the request and
  are never included in the response.

## BLE contract and authorization

Service UUID: `50fc0000-7c91-4d70-8b32-6e856443f501`. Characteristic UUIDs use the same suffix and replace
`0000` with the hexadecimal report ID: `0001`, `0002`, `0003`, `000a`, `000b`, `0014`, `001e`.
Reads return the corresponding payload without a HID report ID. Characteristics use ATT long reads where needed;
the first read freezes the value for subsequent read-blob offsets. Report `0014` has the BLE-specific command
result; writes accept only existing opcode 1 (LED) or 4 (benchmark). Reports are polled; no notification MTU
assumption is needed.

For **every** command:

1. Read `0020` to obtain a new random 16-byte nonce, invalidating any earlier proof.
2. Compute HMAC-SHA256 with the provisioned 32-byte key over `nonce[16] || board_UID[8] || request[9]`.
   UID is the raw identity bytes, in report order; request includes version/transaction/opcode/flags/arguments.
3. Write the first 16 MAC bytes to `0021`, then write that exact request to `0014`.
4. Read the original matching result; never replay SET following timeout/disconnect.

The device consumes a challenge/proof once, compares MAC bytes without early exit, and rate-limits failed proofs
for two seconds. A missing, rejected or rate-limited proof returns ATT application error `0x80`, not Insufficient
Authentication (`0x05`), which would make the central start OS pairing and drop the link. Fresh nonces prevent
replay; UID binding prevents cross-board reuse; request binding prevents
changing an authorized command. This is application authorization, **not encrypted BLE transport** or protection
against radio jamming/connection denial. Use USB for privileged management and credentials.
