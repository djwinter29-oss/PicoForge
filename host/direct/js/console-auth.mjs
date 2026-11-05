export const CONSOLE_AUTH_CHANNEL = "picoforge-direct-console-auth-v1";
export const CONSOLE_AUTH_REQUEST = "tcp-cli-auth-request";
export const CONSOLE_AUTH_RESPONSE = "tcp-cli-auth-response";
export const CONSOLE_AUTH_ERROR = "tcp-cli-auth-error";

export function parseHexBytes(value, length, label = "TCP CLI authentication field") {
  if (typeof value !== "string" || value.length !== length * 2 || !/^[a-f0-9]+$/i.test(value)) {
    throw new Error(`Invalid ${label}`);
  }
  return Uint8Array.from(value.match(/../g), byte => parseInt(byte, 16));
}

export function bytesToHex(bytes) {
  return [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
}

export function randomHex(length = 16) {
  return bytesToHex(globalThis.crypto.getRandomValues(new Uint8Array(length)));
}
