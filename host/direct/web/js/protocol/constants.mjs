/** Firmware-defined values; see docs/reference/usb-hid.md and docs/reference/bluetooth.md. */
export const DEVICE_FILTER = Object.freeze({ vendorId: 0xcafe, usagePage: 0xff00, usage: 1 });
export const PRODUCT_IDS = Object.freeze([0x4011, 0x4012]);
export const CAP = Object.freeze({
  LED: 1, REBOOT: 2, SETTINGS: 4, SPI: 8, SDIO: 16, SELF_TEST: 32, FORCED_SAVE: 64, WIRELESS: 128,
});
export const OP = Object.freeze({
  LED: 1, REBOOT: 2, MSC: 3, SELF_TEST: 4, SAVE_SETTINGS: 5, STAGE_SSID: 6, STAGE_PASSWORD: 7, STAGE_KEY: 8,
  TEST_WIFI: 9, CANCEL_TEST: 10, SCAN: 11, SCAN_ENTRY: 12, TCP_CLI_PROOF: 13,
});
export const CONFIG_FLAGS = Object.freeze({
  CARD_DETECT: 0x01, LED: 0x02, MONITOR_UART: 0x04, WIFI: 0x08, BLE: 0x10, TCP_CLI: 0x20,
  VALID_MASK: 0x3f, RADIO_MASK: 0x18, WIRELESS_MASK: 0x38,
});
export const CLOCKS_MHZ = Object.freeze([0, 150, 200, 250]);
export const BLE_OPCODES = Object.freeze([OP.LED, OP.REBOOT, OP.SAVE_SETTINGS]);
export const SAVE_SETTINGS_FORCE_MSC = 1;
export const WIRELESS_TEST_DETAILS = Object.freeze([
  "", "Radio initialization failed", "Network not found", "Password rejected",
  "No DHCP address", "Timed out", "Test connection lost",
]);
