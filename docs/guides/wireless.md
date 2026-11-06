# Wireless Setup and Operation

Wi-Fi and Bluetooth LE are optional on Pico W and Pico 2 W; their radio switches default to enabled. Wi-Fi normally
joins its saved network, while the dashboard can temporarily test a candidate profile. BLE can advertise only when
enabled with a saved Control key. Other board images do not provide wireless controls. Radio switches and persisted
credentials are boot settings. See the
[settings design](../reference/settings.md) for persistence and save behavior.

## Configuration and operation

Use USB to provision wireless settings. In Python and .NET, open the selected board's **Boot configuration**. In the
Direct dashboard, open **Board configuration**. Use **Configure…** to scan/test Wi-Fi or create a Control key; the
radio switches are separate next-boot controls.

### Wi-Fi

Select a scanned network or enter a hidden SSID, enter its password if needed, and test the connection. Only a
successful test stages credentials, and they are saved to flash only by **Save&Reboot**. Staged test credentials
expire after ten minutes; canceling discards them and returns to the saved network. If the test succeeds, save before
the expiry or test again.

Use a 2.4 GHz network with DHCP. Supported authentication methods and connection retry/fallback behavior are in
[Wi-Fi station behavior](../development/wireless-integration.md#wi-fi-station-behavior).

### Bluetooth LE

Create a Control key and copy it to a password manager before saving; the board never returns the key. Bluetooth
remains off until a key is saved. To connect, use the Direct dashboard's **Bluetooth** option, enter that key, and
select the board. Use a desktop Chrome or Edge browser with Web Bluetooth support, served from localhost or HTTPS.
Python and .NET dashboards can provision Bluetooth settings over USB but do not connect over BLE.

The Control key authenticates commands; it is not a Bluetooth pairing password and does not encrypt the link. See the
[Bluetooth LE protocol](../reference/bluetooth.md) for the complete access model and protocol.

### Save and dashboard differences

**Save&Reboot** commits radio switches and staged credentials with the other boot settings, then reboots after
verified persistence. Disabling a configured radio erases its saved credentials. Credential changes require the
ordinary save path and cannot use forced save while the host owns USB storage; safely eject first. See the
[forced-save policy](../reference/settings.md#forced-save-and-reboot).

Python and .NET manage the selected USB-attached board and do not connect over BLE. Direct supports USB and BLE, with
a restricted set of actions over BLE. See the [Direct dashboard guide](../../host/direct/README.md).

Live diagnostics report wireless state; an enabled switch does not mean the radio is connected. The optional TCP CLI
is a separate service, not a Wi-Fi dashboard connection. It requires Wi-Fi, its saved enable switch, and a Control
key. TCP is unencrypted, so use it only on a trusted network; see the [console guide](console.md#wi-fi-tcp-terminal).
The Control key authenticates commands but does not encrypt BLE; credentials are stored unencrypted in flash. See
the [Bluetooth protocol](../reference/bluetooth.md) and [settings design](../reference/settings.md#record-format).

## Qualification

Host checks and target builds do not qualify physical radio behavior. Follow the
[wireless and BLE board checks](../development/firmware-testing.md#wireless-and-ble-board-checks) and review current
[qualification gaps](../validation/qualification-gaps.md) before relying on a hardware configuration.
