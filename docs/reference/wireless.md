# Wireless Integration and Protocol

This reference owns radio execution, credential persistence, USB provisioning reports, and BLE authorization.
For user setup and restrictions, see [the wireless guide](../guides/wireless.md).

## Execution and storage safety

The application groups radio-specific code under `firmware/src/app/wireless`: `wifi/` contains `wifi.c/.h` for station
connection and reconnects, `mdns.c/.h` for saved-network hostname discovery, and `tcp_cli.c/.h` for the raw TCP console.
`bluetooth/` contains `bluetooth.c/.h` for BLE GATT and authorization state plus `bluetooth_profile.gatt` for the
service definition. `wireless_auth.c/.h` at the wireless root owns the shared key, nonce, board UID and
protocol-separated proof verification. The common `wireless.c/.h` owns SDK initialization/polling, wireless
status, staged Feature `20` commands, and atomic settings/credential saves.

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
complete defaults with no credentials, so Wi-Fi and BLE stay off until provisioned. Feature `20` opcodes `6`-`8`
stage credentials in RAM; opcode `5` saves them only when its explicit commit flag is set, together with the clock,
SD profile, and service switches. The single verified record prevents partial persistent updates.
Flash remains **unencrypted**: physical/debug access can recover credentials. Erasing settings clears them.
Passwords and keys are never included in status, diagnostic logs or readback reports.

## USB contract

Report `1` bit 7 denotes wireless capability. Report `2` `board_flags` bits 3-5 contain the persisted Wi-Fi, BLE,
and TCP CLI switches. Feature `20` owns wireless commands and their immediate results; Feature `12` owns live
wireless, test, and scan status. All wire layouts and opcode arguments are defined by the
[USB HID report reference](usb-hid.md); this document describes lifecycle and security behavior.

- Feature `20` opcodes `6`-`8` stage the SSID, Wi-Fi password, and Control key in RAM. Op `5` saves them only when
  its explicit staged-wireless-commit flag is set, and commits them atomically with the clock, SD profile and
  service switches. The verified save schedules reboot. Secrets are never returned in a result or status report.
- SSIDs are valid UTF-8 of at most 32 bytes. Passwords are printable ASCII: empty for an open network, or 8-63
  bytes. A 64-character hexadecimal raw PSK is not supported. The Control key is exactly 32 raw bytes; length zero
  clears it.
- Feature `20` op `9` tests a staged candidate without saving it. A new SSID cannot be tested or committed with the
  saved password; stage a password too. When the SSID is unchanged, a staged password may use that SSID. Testing
  only saved credentials is not a provisioning test. A successful test is valid only for the exact pair tested;
  restaging either field invalidates it. Staged values expire ten minutes after the last setter; successful testing
  restarts the ten-minute save window. Restaging after a successful test invalidates it and restarts the staged-value
  lifetime; a new successful test starts a new save window.
- A saved password may be used only when the target SSID exactly matches the saved SSID. A scan and a connection
  test are mutually exclusive. Starting another scan or test, or committing staged Wi-Fi values, while one is
  active returns Busy; op `12` returns Busy until scan results are complete.
- When saving a tested pair, persist the authentication mode proven by the test. For a changed SSID saved without
  testing, reset to the normal WPA2-first behavior. If only the password changes for the same SSID, preserve the
  saved authentication mode.
- Op `10` cancels a test and discards staged values. Staging while a test is active returns Busy. Clearing the
  Control key also disables BLE; if TCP CLI remains enabled, it is unauthenticated on the saved LAN, so the host
  must make that consequence explicit before confirming.
- Op `11` starts a scan and returns after acceptance. Feature `12` reports progress and the 16-bit scan generation;
  op `12` reads an entry by index and generation. Results contain at most 18 strongest unique SSIDs and expose only
  SSID, RSSI and security bits.
- Op `13` requests a proof for one fixed TCP CLI admission challenge. It is not a general signing API and never
  exposes the Control key. The proof is one-use at the TCP CLI and cached in the command result for at most ten
  seconds or until another command is accepted.
- Wireless credential, test, scan and proof commands are USB-only. BLE Feature `20` has a 9-byte form that permits
  only LED, confirmed reboot and settings save; an authenticated BLE save may change the TCP CLI switch but must
  preserve both radio switches. Forced saves and wireless credential commits are USB-only.
- Native-host `GET /api/wireless` returns sanitized status. Confirmed `POST /api/wireless` stages provided
  credentials with ops `6`-`8`, optionally tests with op `9`, then commits through op `5` with the explicit
  wireless-commit flag. Omitted credentials are not staged. `POST /api/wireless/scan` uses op `11` and reads each
  result through op `12`; `POST /api/wireless/test` and `/cancel` use ops `9` and `10`. Passwords and keys are
  accepted only in requests and are never included in responses.

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
suffix and replace `0000` with the hexadecimal report ID: `0001`, `0002`, `0003`, `000a`, `000b`, `000c`, `0014`.
Reads return the corresponding payload without a HID report ID. Characteristics use ATT long reads where needed;
the first read freezes the value for subsequent read-blob offsets. Report `0014` has the BLE-specific 9-byte
command/result form; writes accept opcode 1 (LED), 2 (confirmed reboot), or 5 (save settings). The BLE form does not
support wireless staging, test, scan, or TCP CLI proof, and does not use a long write or require a larger MTU.
Reports are polled; no notification MTU assumption is needed.

Each connection starts locked: only the GAP name, `0001` identity (it carries the UID the proofs bind) and the
`0020` nonce are readable; other reads and all writes return `0x80`. To unlock, read `0020`, then write the first
16 bytes of HMAC-SHA256 over `nonce[16] || board_UID[8] || "PFBLECTL1" || "SESSION01"` to `0021`. A rejected or
rate-limited unlock returns `0x80`. The unlock lasts until disconnect; it then opens all reads, including `000c`
status with the SSID and IPv4 address.

On an unlocked link, opcode 1 (LED) only identifies the board, so it needs no further proof: write the request to
`0014` and read the result. An LED write leaves any pending nonce, proof, and rate limit unchanged. Reboot and
settings commands require a fresh proof each time.

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
credentials or Control key. BLE saves may change the TCP CLI switch but must preserve the Wi-Fi and BLE radio
switches; credential commits and forced saves remain USB-only. USB mass-storage ownership stays USB-only.

The TCP CLI uses a separate domain (`"PFTCPCLI1"`) and request (`"TCPCLI001"`). Whenever a Control key is saved,
each connection receives a one-use nonce and board UID challenge and must answer with the first 16 bytes of the
matching HMAC-SHA256 proof. Proofs expire after ten seconds; rejected proofs are rate-limited for two seconds.
Without a key the console is open on the saved network. Python/.NET dashboards ask the selected board to produce
this fixed proof through Feature `20` opcode `13` after checking that the challenge UID matches that board. The Direct
console asks its matching dashboard tab for the proof over a same-origin `BroadcastChannel`; the dashboard checks
the challenge UID, then uses Feature `20` opcode `13` over USB or signs with the non-extractable key from its
already-unlocked BLE session. Only the
one-use proof crosses to the console and local bridge; the Control key is not sent there. If no matching Direct
dashboard is connected, the console refuses to connect rather than prompting for the key. Standalone TCP clients
must compute the proof themselves. Feature `20` opcode `13` does not provide a general signing operation. See the
[console guide](../guides/console.md#direct-dashboard-authentication) for the rationale and local-machine
considerations.
Authenticated or not, the listener accepts one CLI client, closes sessions idle for 15 minutes, and is exposed only
on the saved Wi-Fi network. The TCP stream itself is not encrypted.
