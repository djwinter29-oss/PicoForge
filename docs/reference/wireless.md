# Wireless Integration and Protocol

This reference owns radio execution, credential persistence, USB provisioning reports, and BLE authorization.
For user setup and restrictions, see [the wireless guide](../guides/wireless.md).

## Execution and storage safety

The application groups radio-specific code under `firmware/src/app/wireless`: `wifi/` contains `wifi.c/.h` for station
connection and reconnects, `mdns.c/.h` for saved-network hostname discovery, and `tcp_cli.c/.h` for the raw TCP console.
`bluetooth/` contains `bluetooth.c/.h` for BLE GATT and authorization state plus `bluetooth_profile.gatt` for the
service definition. `wireless_auth.c/.h` at the wireless root owns the shared key, nonce, board UID and
protocol-separated proof verification. The common `wireless.c/.h` owns SDK initialization/polling, combined status
and atomic USB provisioning.

SDK poll-mode wireless work runs on the existing USB owner task. It creates no additional FreeRTOS tasks, preserves
the static-allocation policy, and never invokes FatFs from a radio callback. On W images that owner task is pinned to
its startup core before initializing the SDK at boot: poll-context checks and CYW43 IRQ ownership require it. The same
task drives the CYW43 LED. Non-wireless builds keep the existing all-core task affinity. BLE controls reuse the existing LED
and benchmark admission helpers. USB and BLE retain separate command-result slots, with the original benchmark
admission tied to its source. Storage ownership remains exclusive.

All supported boards advertise LED control. `config/board.c` selects direct GPIO for Pico/Pico 2 and a CYW43 writer
for W boards at build time. The wireless owner initializes the chip and clears its LED even with both radios disabled.
LED writes fail explicitly if that initialization failed; LED support
is not a dynamically detected capability. HID capability and telemetry fields advertise the LED behavior in the
current report contract.

SDK BTstack's default flash-TLV initializer is interposed to select its nonpersistent TLV implementation before
any bank access. Its default Pico W flash banks otherwise overlap the settings sector. No OS bonds are persisted;
application proof authorization does not rely on Bluetooth pairing. Only the existing boot configuration service
writes flash; persistent Bluetooth bonding is not supported.

The version-1 [settings record](settings.md#record-format) stores the clock and SD profiles, the service switches
(byte 10 bits 0/1 disable Wi-Fi/BLE and bit 2 disables the TCP CLI), the saved Wi-Fi authentication state, SSID,
passphrase and Control key under one CRC. The CLI switch defaults enabled for erased records, is independent of the
radio bits returned by Report 2, and is preserved by ordinary clock/speed/radio updates. A bad or torn record loads
complete defaults with no credentials, so Wi-Fi and BLE stay off until provisioned. A Feature 31 update stages the
clock, SD profile, radio and TCP CLI switches and all credentials and saves once; any rejection or failure discards
the staged values, so no partial change survives.
Flash remains **unencrypted**: physical/debug access can recover credentials. Erasing settings clears them.
Passwords and keys are never included in status, diagnostic logs or readback reports.

## USB contract

Identity capability bit 7 denotes wireless provisioning. The radio master switches use Report 2 `board_flags`
bits 3-4 and opcode 6 argument byte 3; see [USB HID report reference](usb-hid.md). Feature 30
reports the saved radio switches; its separate booleans indicate whether credentials are configured.

- Feature **30 GET**, version `1` is 53 payload bytes: version at 0, saved radio-switch flags at 1 (Wi-Fi bit 0, BLE bit
  1), last
  provisioning status at 2 (existing command-status values), transaction at 3, Wi-Fi state at 4
  (disabled/connecting/connected/failed = 0/1/2/3, including temporary tests), saved-network IPv4 bytes at 5-8
  (zero on test networks or without a saved link), NUL-terminated saved SSID at 9-41,
  password/key-configured booleans at 42/43, error at 44 (none/init/join/timeout = 0/1/2/3; a radio init failure
  takes precedence over Wi-Fi join errors until a later start succeeds), BLE link at 45
  (off/advertising/connected = 0/1/2; off until the controller is working), TCP CLI serving boolean
  at 46 (listener started and the saved network joined; zero during a connection test and without a link),
  service flags at 47 (bit 0 disables the saved-LAN TCP CLI; bit 1 requires HMAC
  authentication, set whenever a control key was loaded at boot),
  saved-network auth mode at 48, little-endian reconnect count at 49-50, signed RSSI at 51 (`127` unavailable), and
  restart-required at 52 (`1` when saved wireless settings differ from boot-applied values).
- Feature **31 SET**, 136 payload bytes: version `1` at 0, nonzero transaction at 1, radio switches at 2 (Wi-Fi
  bit 0, BLE bit 1), flags at 3 (preserve password bit 0, preserve key bit 1, use successfully tested Wi-Fi
  credentials bit 2, disable TCP CLI bit 3; a cleared bit 3 enables it), SSID at 4-36, password at 37-100,
  key at 101-132, little-endian clock MHz at 133-134
  (opcode 6 values) and SD profile at 135. A cleared preserve flag with an empty password or all-zero key erases
  that credential. Unknown flags,
  other versions or radio flags inconsistent with the settings are rejected.
  There is **no Feature 31 readback**; hosts confirm the board fields through Report 2.
- Feature **34 SET**, 101 payload bytes: version `1` at 0, operation at 1 (scan `1`, select page `2`, test join
  `3`, clear staged test `4`), nonzero transaction at 2, page at 3, SSID/password byte lengths at 4/5, SSID bytes
  at 6-37 and temporary password at 38-100. Unused fields are zero. Password data is never returned.
  Clear leaves the station link and rejoins the saved network only when a test owns the link; otherwise the saved
  connection is untouched. New scans and tests are rejected while a driver scan is still running.
- Feature **35 GET**, 222 payload bytes: version `1`, transaction, operation state, page/detail, total scan count and
  Wi-Fi state at offsets 0-5. Byte 3 is the selected page (0-2) in scan states and the test detail in states 4-6:
  none `0`, searching `1`, checking password `2`, waiting for DHCP `3`, network not found `4`, password rejected
  `5`, no DHCP address `6`, timed out `7`, connection start failed `8`, tested link lost `9`, staged credentials
  expired `10`. Six entries of 35 bytes each start at 6:
  SSID length, a 32-byte SSID, signed RSSI and the SDK security bitmask (privacy bit 0, WPA bit 1, RSN bit 2).
  RSN means WPA2/WPA3-family advertisement, not verified WPA3 support; enterprise modes are not distinguished.
  Bytes 216-219 contain the IPv4 address only after a
  successful test join. Operation states are idle `0`, scanning `1`, scan complete `2`, scan failed `3`, joining
  `4`, joined `5`, and failed `6`. Bytes 220-221 contain remaining staged-test lifetime in seconds (0-600); it
  remains available after test-link loss (state 6/detail 9). The scan list is capped at 18 strongest unique SSIDs
  (three pages). A scan that
  does not complete within 10 seconds reports scan failed; the driver must finish before releasing station mode or
  retrying the saved network. If the tested link drops, joined becomes failed/detail `9`; credentials stay staged
  until ten minutes after the successful test, then are wiped and cannot be saved (failed/detail `10`).
  Expiry restores the saved network when enabled or releases station mode otherwise. It does not interrupt an
  active scan or overwrite its result; saving expired credentials is still rejected.
  A failed test with saved Wi-Fi disabled reports station disabled/error none; its failure remains in this report.
- Feature **36 SET/GET** provides one narrowly scoped TCP CLI proof over USB. Both payloads are 19 bytes. SET contains
  version `1`, a nonzero transaction, the 16-byte challenge nonce, and a zero reserved byte. GET returns version,
  matching transaction, status (`1` for proof available), and the first 16 HMAC-SHA256 bytes for the fixed
  `"PFTCPCLI1"`/`"TCPCLI001"` request. The proof result is cleared after it is read. This report does not expose
  the Control key or sign arbitrary data; USB HID access can request only a proof for TCP CLI admission.
- Native-host `GET /api/wireless` returns `{wireless, reboot_pending}` with sanitized fields. Confirmed
  `POST /api/wireless` accepts `wifi_enabled`, `ble_enabled` (credential intent), `ssid`, optional `password`,
  optional `tcp_cli_enabled`, optional 64-hex-digit `ble_key`,
  required `board` (the `/api/settings` clock, profile and radios object) and
  required `confirm_reboot:true`. It returns `{wireless, settings, reboot_pending}` after verifying both reports.
  Omitted secrets are preserved; secrets are never returned.
  Existing board query selection and error handling apply.
- `POST /api/wireless/scan` returns nearby sanitized SSIDs, RSSI and security flags. `POST /api/wireless/test`
  accepts an SSID and temporary password and returns join state/IP without saving credentials.
  `POST /api/wireless/cancel` clears a successful temporary test. Passwords are accepted only in the request and
  are never included in the response.

## Saved-network service discovery

The pinned SDK `pico_lwip_mdns` responder answers IPv4 hostname queries for `picoforge-<USB serial>.local` only on
the saved network. It is removed before a credential test or station shutdown and re-registered after reconnect.
While the TCP CLI listens, DNS-SD advertises `_picoforge._tcp` on port 2323 with TXT values `v=1` and
`auth=0`/`auth=1` for whether a control key requires authentication. No outbound resolver is enabled. Registration and
listener allocation failures are logged and retried at five-second intervals. Name conflicts are logged rather than
silently changing the device identity. mDNS requires multicast support on the LAN and an mDNS-capable host resolver;
direct IPv4 access remains available.

The integration cancels the pinned responder's five additional IPv4 timers before removing its netif, since its
removal API cancels only probing. Retained truncated queries are disabled: their private callbacks cannot be canceled
through this pinned API. Ordinary hostname queries are supported, but multi-packet truncated-query handling is not.
Six extra lwIP timer slots bound probing and delayed IPv4 replies. Recheck this teardown when updating the SDK.

## BLE contract and authorization

BLE starts only when its switch is on **and** a control key is saved; enabling it without a key logs a warning and
leaves the BLE link off. Service UUID: `50fc0000-7c91-4d70-8b32-6e856443f501`. Characteristic UUIDs use the same
suffix and replace `0000` with the hexadecimal report ID: `0001`, `0002`, `0003`, `000a`, `000b`, `0014`, `001e`.
Reads return the corresponding payload without a HID report ID. Characteristics use ATT long reads where needed;
the first read freezes the value for subsequent read-blob offsets. Report `0014` has the BLE-specific command
result; writes accept opcode 1 (LED), 2 (confirmed reboot), 4 (benchmark), or 6 (save settings). Reports are polled;
no notification MTU assumption is needed.

Each connection starts locked: only the GAP name, `0001` identity (it carries the UID the proofs bind) and the
`0020` nonce are readable; other reads and all writes return `0x80`. To unlock, read `0020`, then write the first
16 bytes of HMAC-SHA256 over `nonce[16] || board_UID[8] || "PFBLECTL1" || "SESSION01"` to `0021`. A rejected or
rate-limited unlock returns `0x80`. The unlock lasts until disconnect; it then opens all reads, including `001e`
status with the SSID and IPv4 address.

On an unlocked link, opcode 1 (LED) only identifies the board, so it needs no further proof: write the request to
`0014` and read the result. An LED write leaves any pending nonce, proof, and rate limit unchanged. Reboot, benchmark,
and settings commands require a fresh proof each time.

For **every** other command:

1. Read `0020` to obtain a new random 16-byte nonce, invalidating any earlier proof.
2. Compute HMAC-SHA256 with the provisioned 32-byte key over
   `nonce[16] || board_UID[8] || "PFBLECTL1" || request[9]`. UID is the raw identity bytes, in report order;
   request includes version/transaction/opcode/flags/arguments.
3. Write the first 16 MAC bytes to `0021`, then write that exact request to `0014`.
4. Read the original matching result; never replay SET following timeout/disconnect.

The device consumes a challenge/proof once, compares MAC bytes without early exit, and rate-limits failed proofs
for two seconds. A missing, rejected or rate-limited proof returns ATT application error `0x80`, not Insufficient
Authentication (`0x05`), which would make the central start OS pairing and drop the link. Fresh nonces prevent
replay; protocol-domain separation prevents cross-service proof reuse; UID binding prevents cross-board reuse; request
binding prevents changing an authorized command. This is application authorization, **not encrypted BLE transport**
or protection against radio jamming/connection denial. The Direct dashboard can save system clock, SD-card profile,
and the TCP CLI enable switch, and can request a confirmed reboot over BLE. Those settings contain no Wi-Fi
credentials or Control key. BLE saves preserve the saved radio mask; radio switches, credential provisioning, and
forced saves remain USB-only. USB mass-storage ownership stays USB-only.

The TCP CLI uses a separate domain (`"PFTCPCLI1"`) and request (`"TCPCLI001"`). Whenever a Control key is saved,
each connection receives a one-use nonce and board UID challenge and must answer with the first 16 bytes of the
matching HMAC-SHA256 proof. Proofs expire after ten seconds; rejected proofs are rate-limited for two seconds.
Without a key the console is open on the saved network. Python/.NET dashboards ask the selected board to produce
this fixed proof over USB HID after checking that the challenge UID matches that board. The Direct console asks its
matching dashboard tab for the proof over a same-origin `BroadcastChannel`; the dashboard checks the challenge UID,
then uses USB Feature 36 or signs with the non-extractable key from its already-unlocked BLE session. Only the
one-use proof crosses to the console and local bridge; the Control key is not sent there. If no matching Direct
dashboard is connected, the console refuses to connect rather than prompting for the key. Standalone TCP clients
must compute the proof themselves. Feature 36 does not provide a general signing operation. See the
[console guide](../guides/console.md#direct-dashboard-authentication) for the rationale and local-machine
considerations.
Authenticated or not, the listener accepts one CLI client, closes sessions idle for 15 minutes, and is exposed only
on the saved Wi-Fi network. The TCP stream itself is not encrypted.
