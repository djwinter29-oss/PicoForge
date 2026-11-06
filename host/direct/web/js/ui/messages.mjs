// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file The only source of dashboard text for domain reason codes (state/errors.mjs).
 */
import { errorCode, REASON } from "../state/errors.mjs";

/** @brief Display names of the field IDs carried by INVALID_REPORT and INVALID_INPUT. */
export const FIELDS = Object.freeze({
  "identity": "identity", "board-configuration": "board configuration", "telemetry": "telemetry",
  "storage-status": "storage status", "self-test-status": "self-test status", "transaction-id": "transaction ID",
  "command-arguments": "command arguments", "control-request": "control request", "control-result": "control result",
  "transport-result": "transport result", "result-detail": "result detail",
  "completed-result-data": "completed result data", "result-data": "result data",
  "device-selection": "device selection", "led-state": "LED state", "msc-action": "MSC action",
  "self-test-action": "self-test action", "self-test-size": "self-test size", "boot-settings": "boot settings",
  "forced-save": "forced save confirmation", "tcp-cli-setting": "TCP CLI setting",
  "control-key": "Control key (64 hex digits)", "console-nonce": "TCP CLI nonce",
  "console-uid": "TCP CLI board identity", "console-field": "TCP CLI authentication field",
});

/** @brief Lead text for browser DOMException names; other uncoded errors read "Unexpected error". */
/** @brief Feature 12 wireless test detail byte (1-6) to text; 0 means no detail. */
const WIFI_DETAILS = Object.freeze([
  "", "Radio initialization failed", "Network not found", "Password rejected",
  "No DHCP address", "Timed out", "Test connection lost",
]);

/** @brief Text for a firmware enum value the codec could not name (decoded as null). */
export const unknown = (kind, value) => `Unknown ${kind} (${value})`;

const BROWSER = Object.freeze({
  NotAllowedError: "The browser refused the device request", SecurityError: "The browser refused the device request",
  NetworkError: "The board connection failed", InvalidStateError: "The board is not open",
  NotSupportedError: "The browser does not support this device operation",
});

/** @brief English text per reason code: a string, or a function of the code's detail. */
export const MESSAGES = Object.freeze({
  [REASON.COMMAND_BLOCKED]: "Command unavailable or previous outcome uncertain",
  [REASON.USB_ONLY]: "This operation requires USB.",
  [REASON.CAPABILITY_UNAVAILABLE]: "Unavailable capability",
  [REASON.LED_TELEMETRY]: "Fresh LED telemetry unavailable",
  [REASON.STORAGE_IDLE]: "Fresh idle storage status required",
  [REASON.REBOOT_STORAGE]: "Fresh idle firmware-owned storage required",
  [REASON.SELF_TEST_STORAGE]: "Fresh self-test and firmware-owned storage required",
  [REASON.SETTINGS_STORAGE]: "Fresh idle firmware-owned storage required before saving.",
  [REASON.WIRELESS_STORAGE]: "Wireless configuration requires fresh idle USB storage, with sharing disabled.",
  [REASON.REBOOT_CONFIRMATION]: "Reboot requires explicit confirmation",
  [REASON.QUEUE_BUSY]: "Command unavailable while another command owns the queue",
  [REASON.COMMAND_FAILED]: ({ opcode, status, error, detail }) =>
    `Command ${opcode}: status ${status}, error ${error}${detail === undefined ? "" : `, detail ${detail}`}`,
  [REASON.OUTCOME_UNCERTAIN]: "Command outcome uncertain after disconnect",
  [REASON.DISCONNECTED_UNCERTAIN]: "Command outcome uncertain after disconnect; no request will be replayed.",
  [REASON.RESULT_TIMEOUT]: "Command outcome uncertain: result timeout; disconnect/reconnect to read it, never replay",
  [REASON.NOT_CONFIRMED]: "Firmware did not confirm the selected values",
  [REASON.SCAN_MISMATCH]: "Scan entry did not match the requested generation and index",
  [REASON.STORAGE_UNAVAILABLE]: "Session storage is unavailable; refusing an untracked device write.",
  [REASON.RECOVERY_UNAVAILABLE]: detail =>
    `Pending operation recovery data is unavailable or invalid: ${describe(detail)}`,
  [REASON.RECOVERY_CLEAR_FAILED]: detail =>
    `Command was not sent, but recovery state could not be cleared: ${describe(detail)}`,
  [REASON.CONNECTION_CHANGED]: "Board connection changed",
  [REASON.CONNECTION_CANCELLED]: "Connection cancelled",
  [REASON.CLOSE_FAILED]: ({ cause, close }) => `${describe(cause)}; closing the transport failed: ${close}`,
  [REASON.WRONG_BOARD]: "Select the original board to resolve its pending operation.",
  [REASON.STATUS_READ_FAILED]: detail => `Board status read failed: ${describe(detail)}`,
  [REASON.RECOVERY_NOT_CONFIRMED]: "Firmware did not confirm the selected values after reconnect",
  [REASON.RECOVERED_COMMAND]: "Pending command result confirmed after reconnect.",
  [REASON.RECOVERED_CANCEL]: "Wireless test cancellation confirmed after reconnect.",
  [REASON.RECOVERED_WIRELESS]: "Wireless operation confirmed after reconnect.",
  [REASON.WIRELESS_CONNECTION_CHANGED]: "Wireless connection changed",
  [REASON.WIRELESS_UNCERTAIN]: "Wireless command outcome uncertain after disconnect",
  [REASON.WIRELESS_DISCONNECTED]: "Wireless command outcome uncertain; reconnect to inspect its result.",
  [REASON.WIRELESS_TIMEOUT]: "Wireless operation timed out; reconnect to inspect it, never replay",
  [REASON.WIRELESS_STALE]: "Wireless status is stale",
  [REASON.WIRELESS_SCAN_FAILED]: "Wireless scan failed",
  [REASON.SCAN_GENERATION_CHANGED]: "Wireless scan generation changed",
  [REASON.TEST_GENERATION_CHANGED]: "Wireless test generation changed",
  // The firmware phase text names the failure when present.
  [REASON.TEST_FAILED]: detail => WIFI_DETAILS[detail] || "Test credentials expired",
  [REASON.WIFI_DETAIL]: detail => WIFI_DETAILS[detail] || unknown("detail", detail),
  [REASON.PASSWORD_INVALID]: "Password must contain 8-63 printable ASCII characters or be empty for an open network",
  [REASON.PASSWORD_REQUIRED]: "A changed SSID requires an explicit password (empty for an open network)",
  [REASON.SSID_REQUIRED]: "SSID is required for a connection test",
  [REASON.NETWORK_REQUIRED]: "Wi-Fi requires a network",
  [REASON.WIRELESS_CONFIG_INVALID]: "Invalid wireless configuration",
  [REASON.TESTED_PAIR_CHANGED]: "Tested Wi-Fi pair no longer matches; test again",
  [REASON.CONSOLE_CHALLENGE]: "Invalid TCP CLI authentication challenge",
  [REASON.CONSOLE_DISCONNECTED]: "Connect the matching PicoForge board before authorizing its Wi-Fi console",
  [REASON.CONSOLE_WRONG_BOARD]: "The Wi-Fi console challenge belongs to a different board",
  [REASON.CONSOLE_USB]: "Connect the matching PicoForge board over USB to authorize its Wi-Fi console",
  [REASON.CONSOLE_BLUETOOTH]:
    "Connect the matching PicoForge board over Bluetooth to authorize its Wi-Fi console",
  [REASON.INVALID_REPORT]: field => `Invalid HID ${FIELDS[field] ?? field}`,
  [REASON.INVALID_INPUT]: field => `Invalid ${FIELDS[field] ?? field}`,
  [REASON.REPORT_LENGTH]: size => `Expected exactly ${size} HID payload bytes`,
  [REASON.LAYOUT_VERSION]: version => `Unsupported HID layout version ${version}`,
  [REASON.UNKNOWN_PRODUCT]: product => `Unknown product (0x${Number(product).toString(16).padStart(4, "0")})`,
  [REASON.SSID_HEX_LENGTH]: "SSID hex must encode at most 32 bytes",
  [REASON.SSID_MISSING]: "SSID is required",
  [REASON.SSID_LENGTH]: "SSID must be at most 32 UTF-8 bytes",
  [REASON.WIRELESS_STATUS_INVALID]: "Invalid wireless status report",
  [REASON.LIFETIME_INVALID]: "Firmware returned an invalid staged-credential lifetime",
  [REASON.SESSION_STORAGE]: "session storage is unavailable",
  [REASON.JOURNAL_IDENTITY]: "Invalid saved operation identity",
  [REASON.JOURNAL_TRANSACTION]: "Invalid saved control transaction",
  [REASON.JOURNAL_LIFECYCLE]: "Invalid saved wireless lifecycle",
  [REASON.JOURNAL_KIND]: "Unknown saved operation kind",
  [REASON.WEBHID_UNAVAILABLE]: "WebHID is unavailable; use a supported browser on localhost or HTTPS",
  [REASON.BLUETOOTH_UNAVAILABLE]: "Web Bluetooth is unavailable in this browser.",
  [REASON.USB_DISCONNECTED]: "USB device disconnected",
  [REASON.USB_CHANGED]: "USB connection changed",
  [REASON.BLE_KEY_LENGTH]: "Control key must be 32 bytes",
  [REASON.BLE_CHALLENGE]: "The board did not provide a fresh BLE challenge; reconnect and retry.",
  [REASON.BLE_UNLOCK_REJECTED]: "The board rejected the unlock proof. Check the Control key or wait 2 s.",
  [REASON.BLE_PROOF_REJECTED]: "The board rejected or rate-limited the proof. Check the Control key or wait 2 s.",
  [REASON.BLE_FRAME]: "Request does not fit the 9-byte BLE control frame",
  [REASON.BLE_RESULT_LENGTH]: "Expected exactly 9 BLE result bytes",
  [REASON.IO_TIMEOUT]: "Board I/O timeout; command outcome uncertain",
  [REASON.OPERATION_CANCELLED]: "Operation cancelled",
  [REASON.DEVICE_DISCONNECTED]: "Device disconnected",
  [REASON.WIRELESS_CONNECTING]: "Connecting",
  // The browser chooser's cancel needs no diagnostic; everything else keeps the raw message for support.
  [REASON.UNEXPECTED]: ({ name, message }) => name === "NotFoundError" ? "No board was selected."
    : `${BROWSER[name] ?? "Unexpected error"}: ${message}`,
});

/**
 * @brief English text for an error, a reason code or `{code, detail}`; null stays null. A string that is not a
 * code is firmware-provided text (for example a Wi-Fi phase) and is shown verbatim.
 * @param {unknown} value
 * @return {string | null}
 */
export function describe(value) {
  if (value == null) return null;
  // errorCode ignores DOMException's numeric `code`; only BoardError codes and code-valued messages count.
  const { code, detail } = errorCode(value);
  const text = MESSAGES[code];
  return typeof text === "function" ? text(detail) : text ?? String(code);
}
