# Wi-Fi and Bluetooth LE

Pico W and Pico 2 W images include the pinned Pico SDK CYW43, lwIP and BTstack implementations. Non-wireless
images do not link those stacks. A radio starts only when its Board configuration switch is enabled and its
credentials are provisioned. The switches default on (including older settings records); credentials default empty.
This implementation has host checks and target builds, not physical radio qualification.

## Configuration and operation

Connect through USB and expand **Board configuration > Wi-Fi and Bluetooth** on the standalone WebHID or Python/.NET
dashboard. The Wi-Fi and Bluetooth switches sit beside System clock and SD Card on W boards; the collapsed section
holds the credentials.
The server dashboard provisions its selected USB-attached board; the browser does not need Bluetooth access.
Set the SSID and WPA2 passphrase.
SSID length is at most 32 UTF-8 bytes; passphrases are 8-63 printable ASCII characters. Blank password/key fields
preserve the existing secret. Generate a random BLE control key and copy it to a password manager before saving:
the board never returns it. Board configuration **Save and Reboot** commits the clock, SD Card speed, radio switches
and pending credentials together as one atomic save.

Saving credentials requires fresh, idle firmware-owned storage. USB sharing, transitions, benchmark work, outstanding
commands and an uncertain earlier result block provisioning. There is no forced wireless-save operation; a forced
settings save while USB owns storage is available only without credential edits. Cancel or Escape
sends nothing. Save is a single USB SET followed by matching-transaction status polling, never automatic replay.
The existing deferred reboot follows verified persistence; failed saves do not reboot.

Wi-Fi joins an existing 2.4 GHz WPA2 network and obtains an IPv4 address through DHCP. Association/DHCP has a
30-second deadline; failures and dropped connections are reported and retried after five seconds. This does not
provide a web server, network filesystem, Bluetooth Classic, internet management API or Wi-Fi dashboard transport.

The Python and .NET dashboards manage only their USB-attached board and do not offer Bluetooth LE connections.
Their wireless settings panel uses `/api/wireless`; it shares the standalone page's form implementation.
Bluetooth LE connections are available only from the
standalone WebHID page: select **Bluetooth LE** there. The browser must be within radio range and support Web
Bluetooth on localhost or HTTPS (desktop Chrome/Edge where supported by the operating system). USB WebHID
provisioning likewise addresses a board attached to the browser computer.

BLE diagnostics are public. To control the LED or start/cancel a benchmark, enter the USB-provisioned control key
and select **Use BLE key**. It is held only as a non-exportable browser CryptoKey and forgotten on disconnect.
This generated 32-byte key authenticates commands; it is not a Bluetooth pairing password and does not encrypt the link.
BLE cannot save configuration, provision credentials, reboot, or transfer USB storage ownership.
Only one BLE connection is supported; disconnects never replay a command or reconnect automatically.

## Execution and storage safety

The application stays flat under `firmware/src/app/wireless`: `wifi.c/.h` owns station connection and reconnects;
`bluetooth.c/.h` owns BLE GATT and authorization state, with `bluetooth_auth.c/.h` for proof verification and
`bluetooth_profile.gatt` for the service definition. The common `wireless.c/.h` owns shared SDK initialization/polling,
combined status and atomic USB provisioning. Radio-specific modules keep their boot-applied secrets private.

SDK poll-mode wireless work runs on the existing USB owner task. It creates no additional FreeRTOS tasks, preserves
the static-allocation policy, and never invokes FatFs from a radio callback. When a radio is enabled, that owner task
is pinned to its startup core before initializing the SDK: poll-context checks and CYW43 IRQ ownership require it.
Radio-disabled/non-wireless builds keep the existing all-core task affinity. BLE controls reuse the existing LED
and benchmark admission helpers. USB and BLE retain separate command-result slots, with the original benchmark
admission tied to its source. Storage ownership remains exclusive.

SDK BTstack's default flash-TLV initializer is interposed to select its nonpersistent TLV implementation before
any bank access. Its default Pico W flash banks otherwise overlap the settings sector. No OS bonds are persisted;
application proof authorization does not rely on Bluetooth pairing. Only the existing boot configuration service
writes flash. Any future persistent bonding needs explicitly reserved, coordinated storage first.

The unchanged 16-byte version-1 clock/speed record is followed by a 144-byte wireless extension in the same page:
little-endian magic `0x4c574650`, version `1`, size `144`, a reserved byte/SSID[33]/password[64]/key[32], two
reserved zero bytes, and CRC-32 over its first 140 bytes. The first byte was a pair of provisioning flags in earlier
firmware; it is ignored when loading those records and written as zero on the next save. Old records retain
default-enabled master switches but have no provisioned credentials, so neither radio starts.
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
bits 3-4 and opcode 6 argument byte 3; see [USB HID report reference](usb/usb-hid-report-reference.md). Status
report 30 derives its Wi-Fi and BLE bits from the saved SSID and BLE key; they report credential presence, not the
radio master switches.

- Feature **30 GET**, 48 payload bytes: version at 0, credential-presence flags at 1 (Wi-Fi bit 0, BLE bit 1), last
  provisioning status at 2 (existing command-status values), transaction at 3, Wi-Fi state at 4
  (disabled/connecting/connected/failed = 0/1/2/3), IPv4 bytes at 5-8, NUL-terminated SSID at 9-41,
  password/key-configured booleans at 42/43, error at 44 (none/init/join/timeout = 0/1/2/3), zero at 45-47.
- Feature **31 SET**, 136 payload bytes: version `2` at 0, nonzero transaction at 1, radio switches at 2 (Wi-Fi
  bit 0, BLE bit 1), keep-secret flags at 3 (password bit 0, key bit 1), SSID at 4-36, password at 37-100, key at
  101-132, little-endian clock MHz at 133-134 (opcode 6 values) and SD profile at 135. Other versions are rejected.
  There is **no Feature 31 readback**; hosts confirm the board fields through Report 2.
- Native-host `GET /api/wireless` returns `{wireless, reboot_pending}` with sanitized fields. Confirmed
  `POST /api/wireless` accepts `wifi_enabled`, `ble_enabled` (credential intent), `ssid`, optional `password`,
  optional 64-hex-digit `ble_key`, required `board` (the `/api/settings` clock, profile and radios object) and
  required `confirm_reboot:true`. It returns `{wireless, settings, reboot_pending}` after verifying both reports.
  Omitted secrets are preserved; secrets are never returned.
  Existing board query selection and error handling apply.

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
for two seconds. Fresh nonces prevent replay; UID binding prevents cross-board reuse; request binding prevents
changing an authorized command. This is application authorization, **not encrypted BLE transport** or protection
against radio jamming/connection denial. Use USB for privileged management and credentials.

## Qualification

Host checks cover flash migration/validation, HMAC vectors and byte mutations, payload boundaries, missing keys,
restricted BLE writes, single-SET behavior, secret-free status and API errors. The production Wi-Fi state-machine
check covers boot gating, the exact 30-second DHCP deadline, five-second retries, link loss, join failures and IPv4
bytes. Firmware builds cover both W MCUs.
Hardware acceptance still requires both boards, simultaneous Wi-Fi/BLE with SPI and SDIO benchmarks, reconnects,
wrong keys/replays, DHCP timeout, wrong WPA2 password, Wi-Fi loss/retry, USB sharing exclusion, stack high-water
measurements, and confirming that BTstack never mutates the settings sector.
