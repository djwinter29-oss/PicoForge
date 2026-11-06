// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
/** @typedef {{system_clock_mhz: number, sdcard_profile: number, radios: number}} BootSettings */
/**
 * @typedef {{product_name?: string, capabilities?: number, board_model?: string | number, mcu?: string,
 * firmware_version?: string, serial_number?: string, ram_reserved_bytes?: number, ram_total_bytes?: number}} Hardware
 * @typedef {{led_available?: boolean, led_on?: boolean, system_clock_hz?: number, uptime_milliseconds?: number,
 * uptime_ms?: number, cpu_usage_core0_basis_points?: number, cpu_usage_core1_basis_points?: number,
 * cdc_open?: boolean, temperature_valid?: boolean, temperature_centidegrees?: number}} Runtime
 * @typedef {{enabled?: boolean, transition_pending?: boolean, request_rejected?: boolean}} Msc
 * @typedef {{id?: number, manufacturer_id?: number, product_name?: string, product_revision?: string | number,
 * manufacture_year?: number, manufacture_month?: number, serial_number?: number}} Manufacturer
 * @typedef {{initialized?: boolean, configured?: boolean, sample_valid?: boolean, presence_known?: boolean,
 * present?: boolean, bus_clock_hz?: number, capacity_bytes?: number, manufacturer?: Manufacturer | null,
 * msc?: Msc | null, filesystem_format?: string | null}} Memory
 * @typedef {{active?: boolean, state?: string, sequence?: number, progress_percent?: number,
 * write_bytes_per_second?: number, read_bytes_per_second?: number, written_bytes?: number, target_bytes?: number,
 * read_bytes?: number, write_elapsed_ms?: number, read_elapsed_ms?: number, request_error?: number, error?: number,
 * filesystem_result?: number, file_remains?: boolean, mismatch_count?: number}} SelfTest
 * @typedef {BootSettings & {tcp_cli_enabled?: boolean, monitor_uart_index?: number, monitor_uart_tx_gpio?: number,
 * monitor_uart_rx_gpio?: number, storage_gpios?: number[], card_detect_gpio?: number, reset_cause?: number}} BoardConfiguration
 * @typedef {Omit<import("./operation-state.mjs").OperationSnapshot,
 * "hardware" | "runtime" | "memory" | "msc" | "self_test" | "wireless"> & {
 * hardware?: Hardware | null, runtime?: Runtime | null, memory?: Memory | null, msc?: Msc | null,
 * self_test?: SelfTest | null, wireless?: import("./wireless-form.mjs").WirelessStatus | null,
 * board_configuration?: BoardConfiguration | null, error?: string | null, reconnecting?: boolean,
 * wireless_phase?: string | null, control_result?: unknown, diagnostic_error?: string | null}} StatusSnapshot
 */
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

/** Validate facts used for lifecycle transitions before a status reply can replace the last known snapshot.
 * @param {Record<string, unknown>} result
 * @returns {StatusSnapshot}
 */
export function statusResponse(result) {
  for (const field of ["connected", "command_outcome_uncertain", "reboot_pending", "settings_supported",
    "telemetry_stale", "storage_stale", "self_test_stale"]) {
    if (typeof result[field] !== "boolean") throw new DashboardError(`Invalid status field: ${field}`);
  }
  for (const field of ["reconnecting", "command_resync_supported", "wireless_cleanup_required", "outcome_uncertain"]) {
    if (result[field] !== undefined && typeof result[field] !== "boolean")
      throw new DashboardError(`Invalid status field: ${field}`);
  }
  for (const field of ["hardware", "runtime", "memory", "self_test", "board_configuration", "wireless"]) {
    if (result[field] != null && !isObject(result[field])) throw new DashboardError(`Invalid status section: ${field}`);
  }
  /** @type {Array<[unknown, string[]]>} */
  const booleanSections = [
    [result.runtime, ["led_available", "led_on"]],
    [result.memory, ["initialized"]],
    [isObject(result.memory) ? result.memory.msc : null, ["enabled", "transition_pending", "request_rejected"]],
    [result.self_test, ["active"]],
    [result.wireless, ["stale", "wifi_enabled", "ble_enabled", "ble_key_configured", "tcp_cli_enabled",
      "pair_tested", "ssid_staged", "password_staged", "key_staged"]],
  ];
  for (const [section, fields] of booleanSections) {
    if (section == null) continue;
    if (!isObject(section) || fields.some(field => section[field] !== undefined && typeof section[field] !== "boolean"))
      throw new DashboardError("Invalid status boolean facts");
  }
  if (isObject(result.hardware) && (!Number.isInteger(result.hardware.capabilities)
    || typeof result.hardware.capabilities !== "number" || result.hardware.capabilities < 0))
    throw new DashboardError("Invalid status hardware capabilities");
  if (isObject(result.self_test) && (typeof result.self_test.active !== "boolean"
    || typeof result.self_test.sequence !== "number" || !Number.isInteger(result.self_test.sequence)
    || result.self_test.sequence < 0 || result.self_test.sequence > 255))
    throw new DashboardError("Invalid status benchmark facts");
  if (isObject(result.memory) && result.memory.msc != null
    && (!isObject(result.memory.msc) || typeof result.memory.msc.enabled !== "boolean"
      || typeof result.memory.msc.transition_pending !== "boolean"))
    throw new DashboardError("Invalid status storage ownership");
  if (result.board_configuration != null && !isSettings(result.board_configuration))
    throw new DashboardError("Invalid status board configuration");
  if (result.wireless != null) wirelessConfiguration(result.wireless);
  return /** @type {StatusSnapshot} */ (diagnosticFields(result));
}

/** Optional diagnostics degrade field-by-field, with explicit feedback, without changing admitted lifecycle facts.
 * @param {Record<string, unknown>} result @returns {Record<string, unknown>}
 */
function diagnosticFields(result) {
  const numeric = (/** @type {unknown} */ value) => typeof value === "number" && Number.isFinite(value);
  const byte = (/** @type {unknown} */ value) => typeof value === "number"
    && Number.isInteger(value) && value >= 0 && value <= 255;
  const text = (/** @type {unknown} */ value) => typeof value === "string";
  const flag = (/** @type {unknown} */ value) => typeof value === "boolean";
  /** @type {string[]} */
  const invalid = [];
  /** @param {unknown} section @param {string} path
   * @param {Array<[string, (value: unknown) => boolean]>} fields
   */
  const clean = (section, path, fields) => {
    if (!isObject(section)) return section;
    const copy = { ...section };
    for (const [field, valid] of fields) if (copy[field] !== undefined && !valid(copy[field])) {
      invalid.push(`${path}.${field}`);
      delete copy[field];
    }
    return copy;
  };
  /** @param {string} names @param {(value: unknown) => boolean} valid
   * @returns {Array<[string, (value: unknown) => boolean]>}
   */
  const fields = (names, valid) => names.split(" ").map(name => [name, valid]);
  const hardware = clean(result.hardware, "hardware", [
    ...fields("product_name mcu firmware_version serial_number", text),
    ...fields("ram_reserved_bytes ram_total_bytes", numeric),
    ["board_model", value => text(value) || numeric(value)],
  ]);
  const runtime = clean(result.runtime, "runtime", [
    ...fields("system_clock_hz uptime_milliseconds uptime_ms cpu_usage_core0_basis_points "
      + "cpu_usage_core1_basis_points temperature_centidegrees", numeric),
    ...fields("cdc_open temperature_valid", flag),
  ]);
  const memory = clean(result.memory, "memory", [
    ...fields("configured sample_valid presence_known present", flag),
    ...fields("bus_clock_hz capacity_bytes", numeric), ["filesystem_format", value => value === null || text(value)],
  ]);
  if (isObject(memory) && memory.manufacturer != null) {
    if (!isObject(memory.manufacturer)) {
      invalid.push("memory.manufacturer");
      memory.manufacturer = null;
    } else memory.manufacturer = clean(memory.manufacturer, "memory.manufacturer", [
      ...fields("id manufacturer_id", byte),
      ["manufacture_year", value => numeric(value) && Number.isInteger(value)
        && Number(value) >= 2000 && Number(value) <= 2255],
      ["manufacture_month", value => numeric(value) && Number.isInteger(value)
        && Number(value) >= 1 && Number(value) <= 12],
      ["serial_number", value => numeric(value) && Number.isInteger(value) && Number(value) >= 0
        && Number(value) <= 0xffffffff],
      ["product_name", text], ["product_revision", value => text(value) || numeric(value)],
    ]);
  }
  const selfTest = clean(result.self_test, "self_test", [
    ["state", text], ["file_remains", flag],
    ...fields("progress_percent write_bytes_per_second read_bytes_per_second written_bytes target_bytes read_bytes "
      + "write_elapsed_ms read_elapsed_ms request_error error filesystem_result mismatch_count", numeric),
  ]);
  const configuration = clean(result.board_configuration, "board_configuration", [
    ["tcp_cli_enabled", flag],
    ["monitor_uart_index", value => value === 0 || value === 1 || value === 255],
    ...fields("monitor_uart_tx_gpio monitor_uart_rx_gpio card_detect_gpio reset_cause", byte),
    ["storage_gpios", value => Array.isArray(value) && value.every(byte)],
  ]);
  if (!invalid.length) return result;
  return { ...result, hardware, runtime, memory, self_test: selfTest, board_configuration: configuration,
    diagnostic_error: `Unavailable diagnostic fields: ${invalid.join(", ")}.` };
}

/** @param {unknown} wireless @returns {import("./wireless-form.mjs").WirelessStatus} */
export function wirelessConfiguration(wireless) {
  if (!isObject(wireless) || typeof wireless.ssid !== "string"
    || ["wifi_enabled", "ble_enabled", "ble_key_configured", "tcp_cli_enabled"]
      .some(field => typeof wireless[field] !== "boolean")
    || ["stale", "pair_tested", "ssid_staged", "password_staged", "key_staged", "tcp_cli_started"]
      .some(field => wireless[field] !== undefined && typeof wireless[field] !== "boolean")
    || ["wifi_state", "ble_state", "ipv4"]
      .some(field => wireless[field] !== undefined && typeof wireless[field] !== "string")
    || wireless.ssid_hex !== undefined && (typeof wireless.ssid_hex !== "string"
      || !/^(?:[0-9a-f]{2}){0,32}$/i.test(wireless.ssid_hex))
    || wireless.test_generation !== undefined && (typeof wireless.test_generation !== "number"
      || !Number.isInteger(wireless.test_generation)
      || wireless.test_generation < 0 || wireless.test_generation > 255))
    throw new DashboardError("Invalid status wireless configuration");
  return /** @type {import("./wireless-form.mjs").WirelessStatus} */ (wireless);
}

/** Validate the host-confirmed identity and lifetime of a successfully tested candidate.
 * @param {unknown} result
 * @returns {{remaining_seconds: number, test_generation: number, test_id: string, ipv4?: string}}
 */
export function testedWireless(result) {
  if (!isObject(result) || typeof result.remaining_seconds !== "number"
    || !Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 || result.remaining_seconds > 600
    || typeof result.test_generation !== "number" || !Number.isInteger(result.test_generation)
    || result.test_generation < 1 || result.test_generation > 255
    || typeof result.test_id !== "string" || !/^[0-9a-f]{32}$/.test(result.test_id)
    || result.ipv4 !== undefined && typeof result.ipv4 !== "string")
    throw new DashboardError("Invalid wireless test response; outcome is uncertain", undefined, true);
  return { remaining_seconds: result.remaining_seconds, test_generation: result.test_generation,
    test_id: result.test_id,
    ...(typeof result.ipv4 === "string" ? { ipv4: result.ipv4 } : {}) };
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

/** @param {Record<string, unknown>} result
 * @param {import("./wireless-form.mjs").WirelessSave} request
 * @returns {import("./wireless-form.mjs").WirelessConfiguration}
 */
export function savedWireless(result, request) {
  const saved = result.wireless;
  const expectedHex = request.ssid_hex ?? (request.ssid === undefined ? null
    : [...new TextEncoder().encode(request.ssid)].map(byte => byte.toString(16).padStart(2, "0")).join(""));
  if (result.reboot_pending !== true || !isObject(saved) || typeof saved.ssid !== "string"
    || expectedHex !== null && (typeof saved.ssid_hex !== "string"
      || saved.ssid_hex.toLowerCase() !== expectedHex.toLowerCase())
    || saved.wifi_enabled !== request.wifi_enabled || saved.ble_enabled !== request.ble_enabled
    || request.tcp_cli_enabled !== undefined && saved.tcp_cli_enabled !== request.tcp_cli_enabled
    || request.ble_key !== undefined && saved.ble_key_configured !== Boolean(request.ble_key))
    throw new DashboardError("Wireless readback did not match the requested values", undefined, true);
  return /** @type {import("./wireless-form.mjs").WirelessConfiguration} */ (saved);
}

/** @param {Record<string, unknown>} result
 * @returns {Array<{id: string, device_path: string, serial_number?: string | null}>}
 */
export function scannedBoards(result) {
  if (!Array.isArray(result.boards) || !result.boards.every(board => isObject(board)
    && typeof board.id === "string" && board.id.length > 0 && typeof board.device_path === "string"
    && (board.serial_number == null || typeof board.serial_number === "string")))
    throw new DashboardError("Invalid board discovery response");
  return result.boards;
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
