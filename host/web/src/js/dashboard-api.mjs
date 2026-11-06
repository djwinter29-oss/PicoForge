// @ts-check
/** @typedef {{system_clock_mhz: number, sdcard_profile: number, radios: number}} BootSettings */
/** @typedef {{ssid: string, ssid_hex: string, rssi: number, secured: boolean, security?: string}} WirelessNetwork */
/** @typedef {{opcode: number, status: "completed" | "invalid" | "unsupported" | "busy" | "failed",
 * error?: string, detail?: number}} ControlResult */

/** Transport errors keep HTTP refusals distinct from mutations whose completion cannot be established. */
export class DashboardError extends Error {
  /** @param {string} message @param {number} [status] @param {boolean} [uncertain] */
  constructor(message, status, uncertain) {
    super(message);
    this.status = status;
    this.uncertain = uncertain;
    if (uncertain) this.name = "UnknownOutcomeError";
  }
}

/** @param {unknown} value @returns {value is Record<string, unknown>} */
export const isObject = value => value !== null && typeof value === "object" && !Array.isArray(value);

/**
 * @param {Pick<Response, "ok" | "status" | "json">} response
 * @param {string} fallback
 * @param {boolean} [mutation]
 * @returns {Promise<Record<string, unknown>>}
 */
export async function responseJson(response, fallback, mutation = false) {
  /** @type {unknown} */
  let result;
  try { result = await response.json(); }
  catch (error) {
    if (!(error instanceof SyntaxError)) throw error;
    if (response.ok) throw new DashboardError(`Invalid JSON response: ${fallback}`, undefined, mutation);
    result = {};
  }
  if (!response.ok) {
    const body = isObject(result) ? result : {};
    const detail = body.error ?? body.title ?? body.detail;
    const message = typeof detail === "string" ? detail : fallback;
    const uncertain = typeof body.uncertain === "boolean" ? body.uncertain
      : (response.status >= 500 && !/not supported/i.test(message))
        || /uncertain|outcome.*not resent/i.test(message);
    throw new DashboardError(message, response.status, uncertain);
  }
  if (!isObject(result)) throw new DashboardError(`Invalid response: ${fallback}`, undefined, mutation);
  return result;
}

/** @param {unknown} value @returns {value is BootSettings} */
function isSettings(value) {
  return isObject(value) && typeof value.system_clock_mhz === "number" && Number.isInteger(value.system_clock_mhz)
    && typeof value.sdcard_profile === "number" && Number.isInteger(value.sdcard_profile)
    && typeof value.radios === "number" && Number.isInteger(value.radios);
}

/** @param {Record<string, unknown>} result @param {BootSettings} expected @returns {BootSettings} */
export function savedSettings(result, expected) {
  const saved = result.settings;
  if (!isSettings(saved) || saved.system_clock_mhz !== expected.system_clock_mhz
    || saved.sdcard_profile !== expected.sdcard_profile || saved.radios !== expected.radios) {
    throw new DashboardError("Settings readback did not match the requested values", undefined, true);
  }
  return saved;
}

/**
 * Python reports the completed control result; .NET returns an empty acknowledgement.
 * @param {Record<string, unknown>} result @param {number} opcode @returns {ControlResult}
 */
export function acknowledge(result, opcode) {
  if (Object.keys(result).length === 0) return { opcode, status: "completed" };
  const control = result.control_result;
  if (isControlResult(control) && control.status === "completed" && control.opcode === opcode) return control;
  throw new DashboardError("Invalid command acknowledgement; outcome is uncertain", undefined, true);
}

/** @param {unknown} value @returns {value is ControlResult} */
function isControlResult(value) {
  return isObject(value) && typeof value.opcode === "number" && Number.isInteger(value.opcode)
    && value.opcode >= 1 && value.opcode <= 13
    && ["completed", "invalid", "unsupported", "busy", "failed"].some(status => status === value.status)
    && (value.error === undefined || typeof value.error === "string")
    && (value.detail === undefined || typeof value.detail === "number" && Number.isInteger(value.detail)
      && value.detail >= 0 && value.detail <= 255);
}

/** Reconciliation must confirm a terminal outcome before any local obligation is released.
 * @param {Record<string, unknown>} result @returns {ControlResult | null}
 */
export function reconciledResult(result) {
  if (result.uncertain !== false || result.control_result != null && !isControlResult(result.control_result))
    throw new DashboardError("Re-sync did not establish the command outcome", undefined, true);
  return result.control_result == null ? null : result.control_result;
}

/** @param {unknown} value @returns {value is WirelessNetwork} */
function isNetwork(value) {
  return isObject(value) && typeof value.ssid === "string" && typeof value.ssid_hex === "string"
    && /^(?:[0-9a-f]{2}){0,32}$/i.test(value.ssid_hex)
    && typeof value.rssi === "number" && Number.isInteger(value.rssi) && value.rssi >= -128 && value.rssi <= 127
    && typeof value.secured === "boolean" && (value.security === undefined || typeof value.security === "string");
}

/** Validate every scan entry before it can populate selectable credentials.
 * @param {Record<string, unknown>} result @returns {WirelessNetwork[]}
 */
export function scannedNetworks(result) {
  if (!Array.isArray(result.networks) || !result.networks.every(isNetwork))
    throw new DashboardError("Invalid wireless scan response; outcome is uncertain", undefined, true);
  return result.networks;
}
