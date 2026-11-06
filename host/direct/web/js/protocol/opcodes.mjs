/**
 * @file Opcode descriptor table: the single source of every control opcode's capability, link policy, request and
 * result rules, journalled request facts, result confirmation and lifecycle limits. Adding an opcode is one row.
 * See docs/reference/usb-hid.md and docs/reference/bluetooth.md.
 */
import { CAP, CLOCKS_MHZ, OP } from "./constants.mjs";

/** @brief Save-settings bit that releases MSC ownership first; the board may reset USB while saving. */
export const SAVE_SETTINGS_FORCE_MSC = 1;

// radios: bit 0 Wi-Fi, bit 1 Bluetooth; omitted (0) on boards without a radio.
export const validSettings = settings => Boolean(settings) && CLOCKS_MHZ.includes(settings.system_clock_mhz)
  && [0, 1].includes(settings.sdcard_profile) && [undefined, 0, 1, 2, 3].includes(settings.radios)
  && (settings.tcp_cli_enabled === undefined || typeof settings.tcp_cli_enabled === "boolean");
/** @brief Encode save-settings arguments: clock (LE16), SD profile, radios | TCP CLI << 2. */
export const encodeSettings = (settings, tcpCliEnabled) => [settings.system_clock_mhz & 255,
  settings.system_clock_mhz >> 8, settings.sdcard_profile, (settings.radios ?? 0) | Number(tcpCliEnabled) << 2];
/** @brief Decode the four save-settings argument bytes produced by encodeSettings. @param {ArrayLike<number>} args */
export const decodeSettings = args => ({ system_clock_mhz: args[0] | args[1] << 8, sdcard_profile: args[2],
  radios: args[3] & 3, tcp_cli_enabled: Boolean(args[3] & 4) });
const sameSettings = (a, b) => a.system_clock_mhz === b.system_clock_mhz && a.sdcard_profile === b.sdcard_profile
  && (a.radios ?? 0) === (b.radios ?? 0) && a.tcp_cli_enabled === b.tcp_cli_enabled;

const printable = bytes => bytes.every(byte => byte >= 32 && byte <= 126);
const u32 = (report, offset) => new DataView(report.buffer, report.byteOffset).getUint32(offset, true);
const inRange = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;
const settingsKeys = ["system_clock_mhz", "sdcard_profile", "radios", "tcp_cli_enabled"];
const exactKeys = (value, keys) => Boolean(value) && typeof value === "object" && !Array.isArray(value)
  && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));

/**
 * @typedef {object} OpcodeDescriptor
 * @property {number} capability Capability bit the board must report, or 0 when always present.
 * @property {{proof: boolean, flags: boolean} | null} ble Bluetooth policy: null = USB only; `proof` = needs a
 *   per-request HMAC; `flags` = nonzero request flags allowed over Bluetooth.
 * @property {boolean} [reboots] The board reboots after a successful result.
 * @property {(report: Uint8Array, unused: (offset: number) => boolean) => boolean} request Full 68-byte request rule:
 *   [3] flags, [4..] arguments, `unused(n)` = zero tail.
 * @property {number[]} [excluded] Result statuses this opcode never reports.
 * @property {number[]} [rejections] Error reasons allowed with status 3 (default [1, 3]).
 * @property {Record<number, number>} [details] Failure detail code to the one status that may carry it.
 * @property {(data: Uint8Array, zero: (bytes: Uint8Array) => boolean) => boolean} [completed] Completed-result data
 *   rule; default all zero.
 * @property {boolean} [returnsSettings] A completed result carries the saved settings and commit mask.
 * @property {{keys: string[], read: (report: Uint8Array, options: object) => object, valid: (facts: object) =>
 *   boolean}} [facts] Request facts journalled with the transaction and used to confirm its result.
 * @property {(result: object, request: object) => boolean} [confirms] Whether a completed result proves the
 *   requested effect; a mismatch leaves the outcome uncertain.
 * @property {(report: Uint8Array) => boolean} [resetsLink] Whether this request may reset the USB link.
 * @property {{generationMax: number, timeoutMs: number}} [lifecycle] Accepted long-running operation tracked through
 *   Feature 12 after its result.
 */

const STAGED = { excluded: [1] };
/** @type {Readonly<Record<number, OpcodeDescriptor>>} */
export const OPCODES = Object.freeze({
  [OP.LED]: { capability: CAP.LED, ble: { proof: false, flags: true }, excluded: [4, 5],
    request: (r, unused) => r[3] === 0 && r[4] <= 1 && unused(5) },
  [OP.REBOOT]: { capability: CAP.REBOOT, ble: { proof: true, flags: true }, reboots: true, excluded: [4],
    rejections: [1, 2], details: { 1: 5, 2: 5, 28: 5 }, request: (r, unused) => r[3] === 1 && unused(4) },
  [OP.MSC]: { capability: 0, ble: null, excluded: [4], rejections: [1, 2, 3], details: { 1: 5, 2: 5 },
    request: (r, unused) => r[4] <= 2 && r[3] === (r[4] === 2 ? 1 : 0) && unused(5) },
  [OP.SELF_TEST]: { capability: CAP.SELF_TEST, ble: null,
    details: { 1: 5, 2: 5, 3: 3, 4: 6, 5: 6, 6: 6, 7: 6, 10: 6 },
    request: (r, unused) => r[3] === 0 && r[4] <= 1 && unused(9)
      && (r[4] === 0 ? unused(5) : u32(r, 5) >= 1 && u32(r, 5) <= 1024) },
  [OP.SAVE_SETTINGS]: { capability: CAP.SETTINGS, ble: { proof: true, flags: false }, reboots: true,
    details: { 1: 5, 2: 5, 19: 3, 20: 3, 21: 3, 25: 5, 26: 5, 27: 3, 28: 5 }, returnsSettings: true,
    request: (r, unused) => r[3] <= 3 && CLOCKS_MHZ.includes(r[4] | r[5] << 8) && r[6] <= 1 && r[7] <= 7
      && unused(8),
    completed: (data, zero) => validSettings(decodeSettings(data)) && data[3] <= 7 && data[4] <= 7
      && zero(data.subarray(5)),
    facts: { keys: ["settings", "commit_mask"],
      read: (r, options) => ({ settings: decodeSettings(r.subarray(4, 8)), commit_mask: options.commitMask ?? 0 }),
      // Exact keys require a stored radios value, so validSettings alone cannot accept an omitted one.
      valid: facts => exactKeys(facts.settings, settingsKeys) && validSettings(facts.settings)
        && inRange(facts.commit_mask, 0, 7) },
    confirms: (result, request) => sameSettings(result.settings, request.settings)
      && result.commit_mask === request.commit_mask,
    resetsLink: r => Boolean(r[3] & SAVE_SETTINGS_FORCE_MSC) },
  [OP.STAGE_SSID]: { ...STAGED, capability: CAP.WIRELESS, ble: null, details: { 16: 3, 25: 5 },
    request: (r, unused) => r[3] === 0 && r[4] <= 32 && unused(5 + r[4]) },
  [OP.STAGE_PASSWORD]: { ...STAGED, capability: CAP.WIRELESS, ble: null, details: { 17: 3, 25: 5 },
    request: (r, unused) => r[3] === 0 && (r[4] === 0 || r[4] >= 8 && r[4] <= 63)
      && printable(r.subarray(5, 5 + r[4])) && unused(5 + r[4]) },
  [OP.STAGE_KEY]: { ...STAGED, capability: CAP.WIRELESS, ble: null, details: { 18: 3, 25: 5 },
    request: (r, unused) => r[3] === 0 && [0, 32].includes(r[4]) && unused(5 + r[4]) },
  [OP.TEST_WIFI]: { capability: CAP.WIRELESS, ble: null, details: { 19: 3, 20: 3, 21: 3, 25: 5, 26: 5 },
    request: (r, unused) => r[3] === 0 && unused(4), lifecycle: { generationMax: 255, timeoutMs: 35000 } },
  [OP.CANCEL_TEST]: { capability: CAP.WIRELESS, ble: null, excluded: [1, 5],
    request: (r, unused) => r[3] === 0 && unused(4) },
  [OP.SCAN]: { capability: CAP.WIRELESS, ble: null, details: { 25: 5, 26: 5 },
    request: (r, unused) => r[3] === 0 && unused(4), lifecycle: { generationMax: 65535, timeoutMs: 15000 } },
  [OP.SCAN_ENTRY]: { ...STAGED, capability: CAP.WIRELESS, ble: null, details: { 22: 3, 23: 3, 26: 5 },
    request: (r, unused) => r[3] === 0 && r[4] < 18 && (r[5] | r[6] << 8) !== 0 && unused(7),
    completed: (data, zero) => (data[0] | data[1] << 8) !== 0 && data[2] < 18 && data[3] <= 32
      && zero(data.subarray(4 + data[3], 36)) && data[37] <= 7 && zero(data.subarray(38)),
    facts: { keys: ["scan_generation", "scan_index"],
      read: r => ({ scan_index: r[4], scan_generation: r[5] | r[6] << 8 }),
      valid: facts => inRange(facts.scan_generation, 1, 65535) && inRange(facts.scan_index, 0, 17) },
    confirms: (result, request) => (result.data[0] | result.data[1] << 8) === request.scan_generation
      && result.data[2] === request.scan_index },
  [OP.TCP_CLI_PROOF]: { capability: CAP.WIRELESS, ble: null, excluded: [1, 5], details: { 24: 3 },
    request: (r, unused) => r[3] === 0 && unused(20), completed: (data, zero) => zero(data.subarray(16)) },
});

/** @brief Whether the active link may carry this request; Bluetooth carries only the restricted command set. */
export function allowedOnLink(link, opcode, flags = 0) {
  const ble = OPCODES[opcode]?.ble;
  return link !== "ble" || Boolean(ble) && (ble.flags || flags === 0);
}
