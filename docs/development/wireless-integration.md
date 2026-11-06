# Wireless Integration

This document owns shared radio execution, Wi-Fi, credential persistence, provisioning security, and TCP CLI
authorization. Everything specific to Bluetooth is in the [Bluetooth LE protocol](../reference/bluetooth.md).
For user setup and restrictions, see [the wireless guide](../guides/wireless.md).

## Execution and storage safety

The application groups radio-specific code under `firmware/src/app/wireless`: `wifi/` contains `wifi.c/.h` for station
connection and reconnects, `mdns.c/.h` for saved-network hostname discovery, and `tcp_cli.c/.h` for the raw TCP console.
`bluetooth/` holds the [Bluetooth implementation](../reference/bluetooth.md#implementation). `wireless_auth.c/.h` at the
wireless root owns the shared key, nonce, board UID and protocol-separated proof verification. The common
`wireless.c/.h` owns SDK initialization/polling, wireless status, staged Feature `20` commands, and atomic
settings/credential saves.

SDK poll-mode wireless work runs on the existing USB owner task. It creates no additional FreeRTOS tasks, preserves the
static-allocation policy, and never invokes FatFs from a radio callback. On W images that owner task is pinned to its
startup core before initializing the SDK at boot: poll-context checks and CYW43 IRQ ownership require it. The same task
drives the CYW43 LED. Non-wireless builds keep the existing all-core task affinity. Storage ownership remains exclusive.

All supported boards advertise LED control. `config/board.c` selects direct GPIO for Pico/Pico 2 and a CYW43 writer
for W boards at build time. The wireless owner initializes the chip and clears its LED even with both radios disabled.
LED writes fail explicitly if that initialization failed; LED support
is not a dynamically detected capability. HID capability and telemetry fields advertise the LED behavior in the
current report contract.

Only the boot configuration service writes flash; Bluetooth keeps no persistent state (see
[pairing and bonding](../reference/bluetooth.md#pairing-and-bonding)).

The version-2 [settings record](../reference/settings.md#record-format) stores the clock and SD profiles, the service
switches (byte 10 bits 0/1 disable Wi-Fi/BLE and bit 2 disables the TCP CLI), the saved Wi-Fi authentication state,
SSID, passphrase and Control key under one CRC. The CLI switch defaults enabled for erased records, is independent of
the radio bits returned by Report 2, and is preserved by ordinary clock/speed/radio updates. The listener serves only
while a Control key is saved, regardless of the CLI switch; clearing the key disables TCP CLI service rather than
falling back to unauthenticated access. A bad or torn record loads complete defaults with no credentials, so Wi-Fi and
BLE stay off until provisioned. Feature `20` opcodes `6`-`8` stage credentials in RAM; opcode `5` saves them only when
its explicit commit flag is set, together with the clock, SD profile, and service switches. The single verified record
prevents partial persistent updates. Flash remains **unencrypted**: physical/debug access can recover credentials.
Erasing settings clears them. Passwords and keys are never included in status, diagnostic logs or readback reports.

## Provisioning lifecycle

The [USB HID report reference](../reference/usb-hid.md#wireless-command-rules) is the sole source for the wireless wire
contract: Report `1` capability, Report `2` service switches, Feature `12` status, Feature `20` opcodes, credential
limits, staging, testing, expiry, scan, proof and validation. This section adds only radio and security behavior that
those reports do not define.

- Secrets are accepted only by staging commands. They are never returned in a command result, status report,
  diagnostic log or native-host response.
- When saving a tested pair, persist the authentication mode proven by the test. For a changed SSID saved without
  testing, reset to the normal WPA2-first behavior. If only the password changes for the same SSID, preserve the
  saved authentication mode.
- Clearing the Control key prevents the TCP CLI listener from serving until a key is saved again.
- Native hosts report sanitized wireless status as `wireless` in `GET /api/status`. Confirmed `POST /api/wireless`
  stages provided credentials with ops `6`-`8`, optionally tests with op `9`, then commits through op `5` with the
  explicit wireless-commit flag. Omitted credentials are not staged. `POST /api/wireless/scan` uses op `11` and reads
  each result through op `12`; `POST /api/wireless/test` and `/cancel` use ops `9` and `10`.
- `POST /api/control/resync` requires `{"confirm": true}`. It clears a blocked command only when Feature 20 reports
  the exact pending transaction and, for asynchronous Wi-Fi work, Feature 12 reports the matching terminal
  generation. Missing, stale or mismatched results leave writes blocked. Both the Python and .NET dashboards expose
  this route and advertise support through `command_resync_supported`.
- The .NET `/api/boards` response skips duplicate serial/path identity groups and includes a `warnings` array so an
  ambiguous device does not prevent other uniquely identified boards from being selected.

## Wi-Fi station behavior

The station joins existing 2.4 GHz open, WPA2-PSK, WPA2/WPA3 transition, or WPA3-SAE networks and obtains IPv4
through DHCP. WEP, WPA-only, enterprise networks, and 64-hex-digit raw PSKs are unsupported. Scan results cannot
identify SAE, so a connection test with the default authentication state tries WPA2 first. It falls back once to
WPA3-SAE only when the network is not found or association fails before authentication; password rejection and DHCP
timeout do not trigger fallback. After a successful fallback, subsequent attempts use that mode until reboot.

Saved association and DHCP attempts have a 30-second deadline. Failures and dropped links retry with capped
exponential backoff from five to 60 seconds, reset after a successful connection. Scans time out after ten seconds.
The CYW43 driver may report a wrong WPA2 password as repeated handshake timeouts rather than a bad-auth status; a
test fails after two dropped handshakes (about 16 seconds).

A credential test disconnects the existing station before attempting the new network. If the driver refuses the
disconnect request, the firmware retries every five seconds until accepted. Failed tests return to the saved network
as soon as the test link disconnects and do not increase its reconnect backoff. Successful test credentials remain
staged for ten minutes, even if the host disconnects. Expiry wipes the staged passphrase and restores the saved
network, or disables station mode when saved Wi-Fi is off. When saved Wi-Fi is disabled, station mode is enabled
only for a validated scan or test and released after scan completion/timeout, failed test, cancellation, or expiry.
The TCP listener and mDNS responder are tied to the saved station network, never a candidate test network. A
credential test or lost saved link closes the TCP client and suspends new connections until the saved network
reconnects.

## Saved-network service discovery

The pinned SDK `pico_lwip_mdns` responder answers IPv4 hostname queries for `picoforge-<USB serial>.local` only on
the saved network. It is removed before a credential test or station shutdown and re-registered after reconnect.
While the TCP CLI listens, DNS-SD advertises `_picoforge._tcp` on port 2323 with TXT values `v=1` and `auth=1`.
No outbound resolver is enabled. Registration and
listener allocation failures are logged and retried at five-second intervals. Name conflicts are logged rather than
silently changing the device identity. mDNS requires multicast support on the LAN and an mDNS-capable host resolver;
direct IPv4 access remains available.

The integration cancels the pinned responder's five additional IPv4 timers before removing its netif, since its
removal API cancels only probing. Retained truncated queries are disabled: their private callbacks cannot be canceled
through this pinned API. Ordinary hostname queries are supported, but multi-packet truncated-query handling is not.
Six extra lwIP timer slots bound probing and delayed IPv4 replies. Recheck this teardown when updating the SDK.

## TCP CLI authorization

Whenever a Control key is saved, each connection receives the CRLF-terminated line
`PicoForge AUTH <nonce hex> <UID hex>` and must answer `AUTH <proof hex>` plus CRLF, where the proof is the first
16 bytes of `HMAC-SHA256(key, nonce[16] || UID[8] || "PFTCPCLI1" || "TCPCLI001")`. The domain differs from BLE's
`PFBLECTL1` (see the [Bluetooth LE protocol](../reference/bluetooth.md#command-proofs)), so proofs never cross
services. Proofs expire after ten seconds; rejected proofs are rate-limited for two seconds. Without a key the
listener does not serve. Python/.NET dashboards ask the selected board to produce this fixed proof through Feature
`20` opcode `13` after checking that the challenge UID matches that board. The Direct console requests its proof from
the exact same-origin popup opened by its dashboard using `postMessage`; the dashboard checks the challenge UID, then uses
Feature `20` opcode `13` over USB or signs with the non-extractable key from its already-unlocked BLE session. Only
the one-use proof crosses to the console and local bridge; the Control key is not sent there. If no matching Direct
dashboard is connected, the console refuses to connect rather than prompting for the key. Standalone TCP clients
must compute the proof themselves. Feature `20` opcode `13` does not provide a general signing operation. See the
[console guide](../guides/console.md#direct-dashboard-authentication) for the rationale and local-machine
considerations. Authenticated or not, the listener accepts one CLI client, closes sessions idle for 15 minutes, and
is exposed only on the saved Wi-Fi network. The TCP stream itself is not encrypted.
lwIP uses 4-byte heap alignment for Cortex-M packet-buffer stores. Its TCP budget is an 8 KiB heap, three PCBs,
1460-byte MSS, and 2 x MSS send and window buffers.
