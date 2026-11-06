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
/** Report IDs; see the HID report reference. */
export const REPORT = Object.freeze({
  IDENTITY: 1, CONFIG: 2, TELEMETRY: 3, STORAGE: 10, SELF_TEST: 11, WIRELESS: 12, CONTROL: 20,
});
/** Published status reports and how long each sample stays authoritative without a newer one. */
export const REPORT_LIFETIME_MS = Object.freeze({
  [REPORT.TELEMETRY]: 750, [REPORT.STORAGE]: 1500, [REPORT.SELF_TEST]: 1500,
});
/** Control result status byte. Values above PENDING are final. */
export const RESULT = Object.freeze({ IDLE: 0, PENDING: 1, COMPLETED: 2 });
/** Feature 12 `wifi_state`. */
export const WIFI_STATE = Object.freeze({ TESTING: 4, TEST_JOINED: 5, TEST_FAILED: 6 });
/** Feature 12 `scan_state`. */
export const SCAN_STATE = Object.freeze({ IDLE: 0, SCANNING: 1, COMPLETE: 2, FAILED: 3 });
/** Feature 12 `staged_state` bit set when staged wireless values expired. */
export const STAGED_EXPIRED = 8;
