import { bytesToHex } from "../protocol/codec.mjs";

export { bytesToHex, parseHexBytes } from "../protocol/codec.mjs";
export const CONSOLE_AUTH_REQUEST = "direct-console-auth-request";
export const CONSOLE_AUTH_RESPONSE = "direct-console-auth-response";
export const CONSOLE_AUTH_ERROR = "direct-console-auth-error";

export function randomHex(length = 16) {
  return bytesToHex(globalThis.crypto.getRandomValues(new Uint8Array(length)));
}
