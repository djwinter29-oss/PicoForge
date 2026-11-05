import { bytesToHex } from "./protocol.mjs?v=47";

export { bytesToHex, parseHexBytes } from "./protocol.mjs?v=47";
export const CONSOLE_AUTH_CHANNEL = "picoforge-direct-console-auth-v1";
export const CONSOLE_AUTH_REQUEST = "tcp-cli-auth-request";
export const CONSOLE_AUTH_RESPONSE = "tcp-cli-auth-response";
export const CONSOLE_AUTH_ERROR = "tcp-cli-auth-error";


export function randomHex(length = 16) {
  return bytesToHex(globalThis.crypto.getRandomValues(new Uint8Array(length)));
}
