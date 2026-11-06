// @ts-check
/** @typedef {{system_clock_mhz: number, sdcard_profile: number, radios: number}} BootSettings */

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
 * @param {Record<string, unknown>} result @param {number} opcode
 */
export function acknowledge(result, opcode) {
  if (Object.keys(result).length === 0) return;
  const control = result.control_result;
  if (isObject(control) && control.status === "completed" && control.opcode === opcode) return;
  throw new DashboardError("Invalid command acknowledgement; outcome is uncertain", undefined, true);
}
