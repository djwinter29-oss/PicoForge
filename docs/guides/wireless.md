# Wi-Fi and Bluetooth LE

Pico W and Pico 2 W images include the pinned Pico SDK CYW43, lwIP and BTstack implementations. Non-wireless
images do not link those stacks. W images always start the CYW43 chip at boot because it drives the board LED, but a
radio starts only when its Board configuration switch is enabled. Wi-Fi also requires network credentials;
Bluetooth also requires a saved control key and stays off without one.
The switches default on (including older settings records); credentials default empty.
This implementation has host checks and target builds, not physical radio qualification.

## Configuration and operation

Connect through USB and use the Wi-Fi and Bluetooth dropdowns beside System clock and SD Card under Boot
configuration in the Python/.NET dashboard or Board configuration in the Direct dashboard. Each offers
**Enabled**, **Disabled**, and **Configure…**. Configure… opens the Wi-Fi scan/test or Bluetooth key dialog and leaves
the dropdown unchanged; a passed Wi-Fi test or a generated key switches it to Enabled. Wi-Fi Enabled requires a saved
or tested network. Bluetooth Enabled without a key stays off. Switching a saved-enabled radio to Disabled erases
its saved credentials (SSID and password, or control key) on **Save&Reboot**. The server dashboard provisions its
selected USB-attached board; the browser does not need Bluetooth access.

Both dashboards show Wi-Fi in live diagnostics on wireless-capable boards, refreshing the saved SSID and runtime
state every two seconds while no wireless operation is pending. The value is `SSID (IP address)` on the saved network,
`SSID (connecting)`, or `SSID (disconnected)`; Wi-Fi switched off or an empty saved SSID shows `Disabled`.
A Bluetooth row shows the advertised name `PicoForge-<USB serial>` followed by `(connected)` while a central is
attached, `(advertising)` when discoverable, or `(starting)` while the controller is not yet working.
Disabled Bluetooth shows `Disabled`; Bluetooth switched on without a saved control key stays off and shows
`Off (no control key)`.
Over a Direct Bluetooth connection Wi-Fi shows the same `SSID (IP address)` values and console link as USB.
The Direct console reuses the connected dashboard's authentication rather than asking for the Control key again;
see [Direct dashboard authentication](./console.md#direct-dashboard-authentication).
Firmware that predates public BLE status omits the SSID and IP address, so the
row shows only `Connected`, `Wi-Fi (connecting)` or `Wi-Fi (disconnected)`.
The firmware does not report a separate connecting state. If the serial is unavailable, the name is `PicoForge`.
Missing reports show `--`, and read failures show `Unavailable` with the error in the value's tooltip.
The configuration section shows only a staged Wi-Fi test reminder until saved, not duplicate live connection details.

The Wi-Fi dialog scans nearby networks and lets the user select an SSID or enter a hidden one manually. Enter a
password when required and test the connection. Credentials are staged in RAM only after the board joins
successfully; they are not saved to flash until **Save&Reboot**. Cancel or Escape discards staged credentials and
reconnects the saved network only if a test had replaced it; an untested saved connection is left alone.
Successful test credentials expire after ten minutes, even without a dashboard connected. Expiry wipes the staged
passphrase and restores the saved network, or disables station mode when saved Wi-Fi is off. Test again before saving.
Losing the tested link is reported explicitly but does not erase the tested credentials before that deadline.
Scan entries label open, WEP, WPA, and RSN advertisements; RSN includes WPA2/WPA3 and does not prove WPA3 support.
WEP, WPA-only and enterprise networks are not supported for joining.
Connection testing shows the live phase (searching, checking password, waiting for an IP address) and elapsed time;
the firmware deadline is 30 seconds. A failed test names the reason: network not found, password rejected, no DHCP
address, timed out, or the radio could not start the connection. The firmware drops an existing station link before
testing new credentials so the previous network cannot make the test appear successful. If the driver refuses the
disconnect request, firmware retries every five seconds until the driver accepts it. The CYW43 driver reports a wrong
WPA2 password as a handshake timeout and silently re-associates
rather than reporting a bad-auth status, so firmware fails the test after two dropped handshakes (about 16 seconds).
Both server dashboards expose the phase as `wireless_phase` in `/api/status` while a test runs. Python and .NET hosts
continue reading live diagnostics during scan/test polling. Their normal monitors drain nonblocking input batches and
wait outside the HID lock so button requests are not starved. Both dashboards retain the last confirmed card presence
for section visibility when storage reports become stale; stale readings and storage controls remain unavailable.
Controls and dialog dismissal are disabled while a test is pending.
The server dashboard stops waiting after 40 seconds; an uncertain result blocks further writes until the dashboard
is restarted and the board reconnected. Tests are never automatically replayed.
SSID length is at most 32 UTF-8 bytes; passphrases are 8-63 printable ASCII characters, while an empty password
tests an open network. The Bluetooth dialog generates a random control key; copy it to a password manager before
saving because the board never returns it. Bluetooth **Enabled** is unavailable until a key is saved or generated.
There is no separate TCP authentication setting: saving a key requires it for the Wi-Fi CLI, and erasing the key
opens the CLI again. **Save&Reboot** commits the clock, SD Card speed,
radio switches and staged credentials together as one atomic save.

Saving credentials requires fresh, idle firmware-owned storage. USB sharing, transitions, benchmark work, outstanding
commands and an uncertain earlier result block provisioning. There is no forced wireless-save operation; a forced
settings save while USB owns storage is available only without credential edits. Cancel or Escape
sends nothing. Save is a single USB SET followed by matching-transaction status polling, never automatic replay.
The existing deferred reboot follows verified persistence; failed saves do not reboot.

Wi-Fi joins an existing 2.4 GHz open, WPA2-PSK, WPA2/WPA3 transition or WPA3-SAE network and obtains an IPv4
address through DHCP. Scan results cannot identify SAE, so firmware tries WPA2 first. When WPA2 finds no network or
association fails before authentication, a connection test retries once with WPA3-SAE within the same 30-second
deadline; the saved network alternates between WPA2 and SAE after such failures and keeps the mode that worked until
reboot. A rejected password or a DHCP timeout never triggers the fallback. 64-hex-digit raw PSKs are not supported.
Association/DHCP has a 30-second deadline; failures and dropped saved-network connections are reported and retried
with capped exponential backoff from five to 60 seconds, reset after a successful link. A failed connection test
leaves the tested network and rejoins the saved network, if enabled, as soon as the test link disconnects;
it does not increase the saved-network backoff. Scans time out after 10 seconds;
the saved network is rejoined after the scan finishes.
lwIP uses 4-byte heap alignment, required by Cortex-M multi-word stores into received packet buffers. While Wi-Fi
is enabled and the persisted **TCP CLI enabled** switch is on, a single-client raw TCP listener on port 2323 serves
the same CLI as USB CDC; see the
[CLI guide](console.md#wi-fi-tcp-terminal). It is unencrypted and serves only while joined to the saved network:
a connection test, a lost link or a rejoin closes the
client and turns new clients away until the saved network is back. Scan and test commands enable the station
interface only after validation; with saved Wi-Fi disabled, it is released again when the driver scan finishes
(including after its timeout), the test fails or the staged test is cleared or expires. The service switch defaults
on and can be saved off without disabling Wi-Fi or changing its credentials. A saved control key requires TCP
authentication for every session. Without a key, the listener does not serve. Authenticated sessions still use
unencrypted TCP. The normal Wi-Fi
state describes the station, including a test; the status IP is zero on a test network to avoid pairing it with the
saved SSID. Only the test-result report carries the tested address.
The board also answers `picoforge-<USB serial>.local` on the saved network and advertises the CLI via
`_picoforge._tcp` while the listener is available; multicast-capable networking and an mDNS host resolver are
required. No web server is exposed at that name. lwIP TCP is sized for that one client:
1460-byte MSS, 2 × MSS window and send
buffer, three PCBs and an 8 KiB heap. This does not provide a web server, network filesystem, Bluetooth Classic,
internet management API or Wi-Fi dashboard transport.

The Python and .NET dashboards manage only their USB-attached board and do not offer Bluetooth LE connections.
Their wireless settings panel uses `/api/wireless` and their own form implementation. No dashboard components are
shared with Direct; see the [isolation policy](../development/host-tools.md#dashboard-component-isolation).
Bluetooth LE connections are available only from the
Direct dashboard: its **USB** and **Bluetooth** buttons open separate connection dialogs. In the USB dialog,
**Select board** opens the local USB HID picker. In the Bluetooth dialog, **Scan for boards** opens the browser's
nearby-device picker using the browser computer's Bluetooth adapter, filtered to the PicoForge service or name.
Each board advertises the PicoForge service UUID and returns the name `PicoForge-<USB serial>` in its scan response
and GAP Device Name, so the picker can tell boards apart.
The board advertises only when Bluetooth is enabled and a control key is saved. Enter the USB-provisioned control
key before **Scan for boards**; after the link is established the dashboard proves the key once to unlock the session,
and the board refuses every read except identity until it does. A rejected key closes the link and starts a
two-second lockout. The LED needs no further proof; reboot, settings saves, and each benchmark start/cancel are proved
again. The browser must
be within radio range and support Web
Bluetooth on localhost or HTTPS (desktop Chrome/Edge where supported by the operating system). USB WebHID
provisioning likewise addresses a board attached to the browser computer.

The BLE link is not encrypted. Reopen **Bluetooth** to disconnect; use **USB** to disconnect a USB board.
Transport switching is blocked during a connection, reconnection, pending command, or uncertain command outcome.
The key is held only as a non-exportable browser CryptoKey and forgotten on disconnect or a failed connection;
the input is cleared on submission and dialog dismissal.
This generated 32-byte key authenticates commands; it is not a Bluetooth pairing password and does not encrypt the link.
The Direct dashboard can save system clock, SD-card profile, and the TCP CLI enable switch, and request a confirmed
reboot over BLE. It cannot change radio switches, provision Wi-Fi credentials or the Control key, force a save while
USB owns storage, or transfer USB storage ownership. These authenticated settings commands contain no credentials;
the BLE link itself remains unencrypted.
Only one BLE connection is supported; disconnects never replay a command or reconnect automatically.

## Implementation Reference

Radio task ownership, persistence, USB report layouts, native-host APIs, and BLE proof formats live in the
[wireless reference](../reference/wireless.md).

## Qualification

Host checks cover flash-record validation, HMAC vectors and byte mutations, payload boundaries, missing keys,
restricted BLE writes, single-SET behavior, secret-free status and API errors. The production Wi-Fi state-machine
check covers boot gating, the exact 30-second DHCP deadline, capped exponential saved-network retries, link loss, join
failures, the WPA3-SAE fallback and IPv4 bytes, exact ten-minute staged-credential expiry, scan security flags,
delayed scan cleanup, listener retry pacing, and mDNS registration/removal. BLE checks assert one report build per
read and stable read-blob snapshots. Firmware builds cover both W MCUs.
Hardware acceptance still requires both boards, simultaneous Wi-Fi/BLE with SPI and SDIO benchmarks, reconnects,
wrong keys/replays, DHCP timeout, wrong WPA2 and WPA3 passwords, a WPA3-only network join, Wi-Fi loss/retry, USB
sharing exclusion, stack high-water measurements, and confirming that BTstack never mutates the settings sector.
Also check mDNS resolution after DHCP and reconnect, absence on the test network, a name conflict, and disconnect
with delayed mDNS replies pending. Verify the persisted TCP switch blocks clients after reboot, then re-enable it;
exercise staged-test link loss and ten-minute expiry with the dashboard both connected and absent.
