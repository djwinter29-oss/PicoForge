// @ts-check
/**
 * @file The shared failure and notice vocabulary. Protocol, transport, board and timer code throw BoardError codes
 * (with a field ID or numbers as detail), never English; ui/messages.mjs is the only place that turns them into
 * text. This module imports nothing, so every layer may depend on it (enforced by architecture.test.mjs).
 * Errors without a code (browser DOMExceptions, programmer-invariant violations) are stored as REASON.UNEXPECTED
 * with their name and raw message as diagnostic detail.
 */

/** @brief Every domain reason code. Values are stable kebab-case identifiers. */
export const REASON = Object.freeze({
  // Readiness gates (state/selectors.mjs).
  COMMAND_BLOCKED: "command-blocked",
  USB_ONLY: "usb-only",
  CAPABILITY_UNAVAILABLE: "capability-unavailable",
  LED_TELEMETRY: "led-telemetry",
  STORAGE_IDLE: "storage-idle",
  REBOOT_STORAGE: "reboot-storage",
  SELF_TEST_STORAGE: "self-test-storage",
  SETTINGS_STORAGE: "settings-storage",
  WIRELESS_STORAGE: "wireless-storage",
  REBOOT_CONFIRMATION: "reboot-confirmation",
  // Command queue and results.
  QUEUE_BUSY: "queue-busy",
  COMMAND_FAILED: "command-failed",
  OUTCOME_UNCERTAIN: "outcome-uncertain",
  DISCONNECTED_UNCERTAIN: "disconnected-uncertain",
  RESULT_TIMEOUT: "result-timeout",
  NOT_CONFIRMED: "not-confirmed",
  SCAN_MISMATCH: "scan-mismatch",
  STORAGE_UNAVAILABLE: "storage-unavailable",
  RECOVERY_UNAVAILABLE: "recovery-unavailable",
  RECOVERY_CLEAR_FAILED: "recovery-clear-failed",
  // Connection and recovery.
  CONNECTION_CHANGED: "connection-changed",
  CONNECTION_CANCELLED: "connection-cancelled",
  CLOSE_FAILED: "close-failed",
  WRONG_BOARD: "wrong-board",
  STATUS_READ_FAILED: "status-read-failed",
  RECOVERY_NOT_CONFIRMED: "recovery-not-confirmed",
  RECOVERED_COMMAND: "recovered-command",
  RECOVERED_CANCEL: "recovered-cancel",
  RECOVERED_WIRELESS: "recovered-wireless",
  // Wireless lifecycle and status.
  WIRELESS_CONNECTION_CHANGED: "wireless-connection-changed",
  WIRELESS_UNCERTAIN: "wireless-uncertain",
  WIRELESS_DISCONNECTED: "wireless-disconnected",
  WIRELESS_TIMEOUT: "wireless-timeout",
  WIRELESS_STALE: "wireless-stale",
  WIRELESS_SCAN_FAILED: "wireless-scan-failed",
  SCAN_GENERATION_CHANGED: "scan-generation-changed",
  TEST_GENERATION_CHANGED: "test-generation-changed",
  TEST_FAILED: "test-failed",
  WIFI_DETAIL: "wifi-detail",
  // Wireless input.
  PASSWORD_INVALID: "password-invalid",
  PASSWORD_REQUIRED: "password-required",
  SSID_REQUIRED: "ssid-required",
  NETWORK_REQUIRED: "network-required",
  WIRELESS_CONFIG_INVALID: "wireless-config-invalid",
  TESTED_PAIR_CHANGED: "tested-pair-changed",
  // Wi-Fi console authorization.
  CONSOLE_CHALLENGE: "console-challenge",
  CONSOLE_DISCONNECTED: "console-disconnected",
  CONSOLE_WRONG_BOARD: "console-wrong-board",
  CONSOLE_USB: "console-usb",
  CONSOLE_BLUETOOTH: "console-bluetooth",
  // Protocol validation (protocol/codec.mjs); detail is a field ID or the offending number.
  INVALID_REPORT: "invalid-report",
  INVALID_INPUT: "invalid-input",
  REPORT_LENGTH: "report-length",
  LAYOUT_VERSION: "layout-version",
  UNKNOWN_PRODUCT: "unknown-product",
  SSID_HEX_LENGTH: "ssid-hex-length",
  SSID_MISSING: "ssid-missing",
  SSID_LENGTH: "ssid-length",
  WIRELESS_STATUS_INVALID: "wireless-status-invalid",
  LIFETIME_INVALID: "lifetime-invalid",
  // Pending-operation journal (board/commands.mjs).
  SESSION_STORAGE: "session-storage",
  JOURNAL_IDENTITY: "journal-identity",
  JOURNAL_TRANSACTION: "journal-transaction",
  JOURNAL_LIFECYCLE: "journal-lifecycle",
  JOURNAL_KIND: "journal-kind",
  // Transports and timers.
  WEBHID_UNAVAILABLE: "webhid-unavailable",
  BLUETOOTH_UNAVAILABLE: "bluetooth-unavailable",
  USB_DISCONNECTED: "usb-disconnected",
  USB_CHANGED: "usb-changed",
  BLE_KEY_LENGTH: "ble-key-length",
  BLE_CHALLENGE: "ble-challenge",
  BLE_UNLOCK_REJECTED: "ble-unlock-rejected",
  BLE_PROOF_REJECTED: "ble-proof-rejected",
  BLE_FRAME: "ble-frame",
  BLE_RESULT_LENGTH: "ble-result-length",
  IO_TIMEOUT: "io-timeout",
  OPERATION_CANCELLED: "operation-cancelled",
  DEVICE_DISCONNECTED: "device-disconnected",
  // Progress notices.
  WIRELESS_CONNECTING: "wireless-connecting",
  // An error without a code; detail is `{name, message}` from the original error.
  UNEXPECTED: "unexpected",
});

/** @brief Codes whose failure leaves a device write with an unknown outcome; never replay such a request. */
/** @type {Set<string>} */
const UNCERTAIN = new Set([REASON.IO_TIMEOUT, REASON.OUTCOME_UNCERTAIN, REASON.DISCONNECTED_UNCERTAIN, REASON.RESULT_TIMEOUT,
  REASON.NOT_CONFIRMED, REASON.RECOVERY_NOT_CONFIRMED, REASON.WIRELESS_UNCERTAIN, REASON.WIRELESS_DISCONNECTED,
  REASON.WIRELESS_TIMEOUT]);
/** @brief Codes refused before any device write; the user may retry once the condition clears. */
/** @type {Set<string>} */
const RETRYABLE = new Set([REASON.COMMAND_BLOCKED, REASON.QUEUE_BUSY, REASON.LED_TELEMETRY, REASON.STORAGE_IDLE,
  REASON.REBOOT_STORAGE, REASON.SELF_TEST_STORAGE, REASON.SETTINGS_STORAGE, REASON.WIRELESS_STORAGE,
  REASON.STATUS_READ_FAILED, REASON.CONNECTION_CHANGED, REASON.WIRELESS_CONNECTION_CHANGED]);

/** @brief A board-domain failure identified by a reason code; `message` is the code, not display text. */
export class BoardError extends Error {
  /**
   * @param {string} code One of REASON.
   * @param {unknown} [detail] Code-specific detail (for example the failed result fields).
   */
  constructor(code, detail = null) {
    super(code);
    this.name = "BoardError";
    /** Reason code from REASON. */
    this.code = code;
    /** Code-specific detail, or null. */
    this.detail = detail;
    /** The device write's outcome is unknown; recovery must read it, never replay it. */
    this.uncertain = UNCERTAIN.has(code);
    /** Nothing reached the device; retrying after the condition clears is safe. */
    this.retryable = RETRYABLE.has(code);
  }
}

/** @type {Set<string>} */
const CODES = new Set(Object.values(REASON));

/**
 * @brief The `{code, detail}` of any thrown value. Errors whose message is a code (timer DOMExceptions keep their
 * AbortError/TimeoutError name and carry a code as message) map to that code; any other error is UNEXPECTED.
 * @param {unknown} error
 * @return {{code: string, detail: unknown}}
 */
export function errorCode(error) {
  if (error instanceof BoardError) return { code: error.code, detail: error.detail };
  if (typeof error === "string") return { code: error, detail: null };
  if (error && typeof error === "object" && !(error instanceof Error) && "code" in error) {
    return { code: String(error.code), detail: /** @type {{detail?: unknown}} */ (error).detail ?? null };
  }
  const { name = "Error", message = String(error) } = /** @type {{name?: string, message?: string}} */ (error ?? {});
  return CODES.has(message) ? { code: message, detail: null } : { code: REASON.UNEXPECTED, detail: { name, message } };
}

/**
 * @brief Rebuild a throwable error from a state value written by errorValue.
 * @param {string | {code: string, detail: unknown}} value
 */
export function errorFromValue(value) {
  const { code, detail = null } = typeof value === "object" ? value : { code: value };
  return new BoardError(code, detail);
}

/**
 * @brief The serializable state value for an error: a code, or `{code, detail}`. Passing a code string or null
 * returns it unchanged.
 * @param {unknown} error
 * @return {string | {code: string, detail: unknown} | null}
 */
export function errorValue(error) {
  if (error == null) return null;
  const { code, detail } = errorCode(error);
  return detail == null ? code : { code, detail };
}
