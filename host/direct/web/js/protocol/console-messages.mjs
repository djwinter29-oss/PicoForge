/**
 * @file Cross-window message protocol between the dashboard and its Wi-Fi console window: the console asks the
 * dashboard (which owns the board session) to prove TCP CLI authentication for a challenge.
 */
import { bytesToHex } from "./codec.mjs";

export { bytesToHex, parseHexBytes } from "./codec.mjs";
/** @brief Console to dashboard: `{type, request_id, uid, challenge}`. */
export const CONSOLE_AUTH_REQUEST = "direct-console-auth-request";
/** @brief Dashboard to console: `{type, request_id, uid, proof}`. */
export const CONSOLE_AUTH_RESPONSE = "direct-console-auth-response";
/** @brief Dashboard to console: `{type, request_id, error}`. */
export const CONSOLE_AUTH_ERROR = "direct-console-auth-error";

/** @brief Random lowercase hex request identifier from Web Crypto. */
export function randomHex(length = 16) {
  return bytesToHex(globalThis.crypto.getRandomValues(new Uint8Array(length)));
}
