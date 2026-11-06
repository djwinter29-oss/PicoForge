/**
 * Pure version-1 HID report codecs and shared value rules for the Direct dashboard. No browser or device state.
 */
import {
  BLE_OPCODES, CAP, CLOCKS_MHZ, CONFIG_FLAGS, DEVICE_FILTER, OP, PRODUCT_IDS, WIRELESS_TEST_DETAILS,
} from "./constants.mjs";

function payload(report, size) {
  const view = report instanceof DataView ? report
    : report instanceof Uint8Array ? new DataView(report.buffer, report.byteOffset, report.byteLength) : null;
  // WebHID exposes the ID separately; no value-based framing or padding removal.
  if (!view || view.byteLength !== size) throw new Error(`Expected exactly ${size} HID payload bytes`);
  if (view.getUint8(0) !== 1) throw new Error(`Unsupported HID layout version ${view.getUint8(0)}`);
  return view;
}

export function requireValid(condition, label) {
  if (!condition) throw new Error(`Invalid HID ${label}`);
}

// radios: bit 0 Wi-Fi, bit 1 Bluetooth; omitted (0) on boards without a radio.
export const validSettings = settings => Boolean(settings) && CLOCKS_MHZ.includes(settings.system_clock_mhz)
  && [0, 1].includes(settings.sdcard_profile) && [undefined, 0, 1, 2, 3].includes(settings.radios)
  && (settings.tcp_cli_enabled === undefined || typeof settings.tcp_cli_enabled === "boolean");
export const bytesToHex = bytes => [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
/** Decode already-validated even-length hex; use parseHexBytes at trust boundaries. */
export const hexToBytes = hex => Uint8Array.from(hex.match(/../g) ?? [], byte => parseInt(byte, 16));
export function parseHexBytes(value, length, label = "TCP CLI authentication field") {
  if (typeof value !== "string" || value.length !== length * 2 || !/^[a-f0-9]+$/i.test(value)) {
    throw new Error(`Invalid ${label}`);
  }
  return hexToBytes(value);
}
const bytesAt = (view, offset, size) => new Uint8Array(view.buffer, view.byteOffset + offset, size);
const zero = bytes => bytes.every(byte => byte === 0);

/** @brief Encode save-settings arguments: clock (LE16), SD profile, radios | TCP CLI << 2. */
export const encodeSettings = (settings, tcpCliEnabled) => [settings.system_clock_mhz & 255,
  settings.system_clock_mhz >> 8, settings.sdcard_profile, (settings.radios ?? 0) | Number(tcpCliEnabled) << 2];
/** @brief Decode the four save-settings argument bytes produced by encodeSettings. */
export const decodeSettings = args => ({ system_clock_mhz: args[0] | args[1] << 8, sdcard_profile: args[2],
  radios: args[3] & 3, tcp_cli_enabled: Boolean(args[3] & 4) });

export function decodeIdentityReport(report) {
  const view = payload(report, 26);
  const product = view.getUint16(1, true);
  if (product !== 2) throw new Error(`Unknown product (0x${product.toString(16).padStart(4, "0")})`);
  const mcu = view.getUint8(14);
  const board = view.getUint8(15);
  const capabilities = view.getUint8(17);
  const total = view.getUint32(18, true);
  const reserved = view.getUint32(22, true);
  requireValid([8, 16].includes(capabilities & 24)
    && total > 0 && reserved <= total
    && (![1, 4].includes(board) || mcu === 1) && (![2, 3].includes(board) || mcu === 2), "identity");
  return {
    version: 1, product_id: product, product_name: "PicoForge",
    serial_number: bytesToHex(bytesAt(view, 3, 8)).toUpperCase(),
    firmware_version: [11, 12, 13].map(offset => view.getUint8(offset)).join("."),
    mcu_id: mcu, mcu: ({ 1: "RP2040", 2: "RP2350" })[mcu] ?? `Unknown MCU (${mcu})`,
    board_model: board, board_name: ({ 0: "Unspecified", 1: "Pico", 2: "Pico 2", 3: "Pico 2 W", 4: "Pico W" })[board]
      ?? `Unknown board (${board})`,
    capabilities, transport: capabilities & 8 ? "spi" : "sdio",
    ram_total_bytes: total, ram_reserved_bytes: reserved,
  };
}

export function decodeConfigReport(report, identity) {
  const view = payload(report, 17);
  const flags = view.getUint8(13);
  const gpios = [...bytesAt(view, 7, 6)];
  const uart = view.getUint8(4);
  const tx = view.getUint8(5);
  const rx = view.getUint8(6);
  const led = view.getUint8(15);
  const detect = view.getUint8(16);
  const settings = { system_clock_mhz: view.getUint16(1, true), sdcard_profile: view.getUint8(3),
    radios: (flags & CONFIG_FLAGS.RADIO_MASK) >> 3, tcp_cli_enabled: Boolean(flags & CONFIG_FLAGS.TCP_CLI) };
  const maxGpio = [1, 2, 3, 4].includes(identity.board_model) || identity.mcu_id === 1 ? 29
    : identity.mcu_id === 2 ? 47 : -1;
  requireValid(validSettings(settings) && !(flags & ~CONFIG_FLAGS.VALID_MASK)
    && (identity.capabilities & CAP.WIRELESS || !(flags & CONFIG_FLAGS.WIRELESS_MASK))
    && [0, 1, 255].includes(uart)
    && (tx === 255) === (rx === 255) && (!(flags & CONFIG_FLAGS.MONITOR_UART) || (uart !== 255 && tx !== 255))
    && Boolean(flags & CONFIG_FLAGS.CARD_DETECT) === (detect !== 255)
    && Boolean(flags & CONFIG_FLAGS.LED) === (led !== 255)
    && (!(flags & CONFIG_FLAGS.LED) || Boolean(identity.capabilities & CAP.LED))
    && [tx, rx, led, detect, ...gpios].every(pin => pin === 255 || pin <= maxGpio)
    && (identity.transport === "spi"
      ? gpios.slice(0, 4).every(pin => pin !== 255) && gpios.slice(4).every(pin => pin === 255)
      : gpios.every(pin => pin !== 255)), "board configuration");
  return { ...settings, monitor_uart_index: uart, monitor_uart_tx_gpio: tx, monitor_uart_rx_gpio: rx,
    storage_gpios: gpios, board_flags: flags, reset_cause: view.getUint8(14), led_gpio: led,
    card_detect_gpio: detect };
}

export function decodeTelemetryReport(report, identity) {
  const view = payload(report, 22);
  const led = view.getUint8(1);
  const flags = view.getUint8(21);
  const loads = [view.getUint16(12, true), view.getUint16(14, true)];
  const temperature = view.getInt16(2, true);
  const clock = view.getUint32(16, true);
  requireValid(!(led & ~1) && !(flags & ~15) && (!led || Boolean(flags & 8))
    && (!identity || Boolean(flags & 8) === Boolean(identity.capabilities & 1))
    && (Boolean(flags & 4) || temperature === 0) && clock > 0
    && loads.every(load => load <= 10000 || load === 65535), "telemetry");
  return { version: 1, sequence: view.getUint8(20), uptime_ms: view.getBigUint64(4, true),
    system_clock_hz: clock, usb_mounted: Boolean(flags & 1), cdc_open: Boolean(flags & 2),
    temperature_valid: Boolean(flags & 4), temperature_centidegrees: flags & 4 ? temperature : null,
    led_available: Boolean(flags & 8), led_on: Boolean(led),
    cpu_usage_core0_basis_points: loads[0] === 65535 ? null : loads[0],
    cpu_usage_core1_basis_points: loads[1] === 65535 ? null : loads[1] };
}

function decodeCid(cid) {
  const ascii = (start, end) => String.fromCharCode(...cid.slice(start, end)).replace(/[\0 ]+$/, "");
  return { id: cid[0], oem_id: ascii(1, 3), product_name: ascii(3, 8),
    product_revision: `${cid[8] >> 4}.${cid[8] & 15}`,
    serial_number: new DataView(cid.buffer, cid.byteOffset, cid.byteLength).getUint32(9, false),
    manufacture_year: 2000 + ((cid[13] & 15) << 4) + (cid[14] >> 4), manufacture_month: cid[14] & 15 };
}

export function decodeStorageReport(report, identity) {
  const view = payload(report, 34);
  const flags = view.getUint8(2);
  const component = view.getUint8(3);
  const mode = view.getUint8(4);
  const clock = view.getUint32(5, true);
  const speed = view.getUint8(9);
  const blocks = view.getUint32(10, true);
  const cid = bytesAt(view, 14, 16);
  const format = view.getUint8(30);
  const enabled = view.getUint8(31);
  const pending = view.getUint8(32);
  const rejected = view.getUint8(33);
  const configured = Boolean(flags & 1);
  const sampleValid = Boolean(flags & 64);
  const initialized = Boolean(flags & 8);
  requireValid(!(flags & 128) && format <= 4 && [enabled, pending, rejected].every(value => value <= 1)
    && (!enabled || format === 0)
    && (!sampleValid || configured) && (configured || !(flags & 126))
    && (sampleValid || (!(flags & 62) && zero(bytesAt(view, 3, 27))))
    && (!sampleValid || (component === 1 && mode === (identity.transport === "sdio" ? 1 : 2)))
    && (!initialized || (flags & 15) === 15)
    && (!(flags & 48) || (sampleValid && initialized))
    && (!(flags & 4) || Boolean(flags & 2))
    && (initialized ? clock > 0 && [1, 2].includes(speed) : clock === 0 && speed === 0)
    && (Boolean(flags & 32) ? blocks > 0 : blocks === 0)
    && (Boolean(flags & 16) || zero(cid))
    && (!(enabled || pending) || !sampleValid), "storage status");
  return {
    memory: { version: 1, sequence: view.getUint8(1), configured, sample_valid: sampleValid,
      presence_known: sampleValid && Boolean(flags & 2), present: sampleValid && Boolean(flags & 4),
      initialized, mode: sampleValid ? identity.transport : null, speed_mode: speed,
      bus_clock_hz: clock, capacity_bytes: flags & 32 ? blocks * 512 : null,
      cid: [...cid], manufacturer: flags & 16 ? decodeCid(cid) : null },
    filesystem_format: ["unknown", "fat12", "fat16", "fat32", "exfat"][format],
    msc: { enabled: Boolean(enabled), transition_pending: Boolean(pending),
      request_rejected: Boolean(rejected) },
  };
}

export function decodeSelfTestReport(report) {
  const view = payload(report, 32);
  const state = view.getUint8(2);
  const flags = view.getUint8(3);
  const target = view.getUint32(4, true);
  const written = view.getUint32(8, true);
  const read = view.getUint32(12, true);
  const writeMs = view.getUint32(16, true);
  const readMs = view.getUint32(20, true);
  const mismatches = view.getUint32(24, true);
  const error = view.getUint8(28);
  const fs = view.getUint8(29);
  // Unknown enums remain diagnostic data but can never authorize a state transition.
  const enumsKnown = state <= 8 && error <= 10 && fs <= 19;
  requireValid(!(flags & ~3) && view.getUint8(30) === 0 && view.getUint8(31) === 0 && target <= 1073741824
    && written <= target && read <= written && mismatches <= read
    && (error > 10 || error === 0 || [4, 5, 7, 8, 9].includes(error))
    && (state > 8 || error > 10 || (state === 7 ? error !== 0 : error === 0))
    && (state > 8 || fs > 19 || (state === 7 && [7, 9].includes(error) ? error !== 7 || fs > 0 : fs === 0))
    && (state !== 0 || zero(bytesAt(view, 4, 25)))
    && (state !== 1 || (target > 0 && zero(bytesAt(view, 8, 21))))
    && (state === 0 || state > 8 || target > 0)
    && (state !== 6 || (written === target && read === target && !mismatches && !(flags & 1))),
  "self-test status");
  return { version: 1, sequence: view.getUint8(1), state_id: state,
    state: ["idle", "queued", "preparing", "writing", "reading", "cleanup", "passed", "failed", "cancelled"][state]
      ?? `Unknown state (${state})`,
    valid_for_control: enumsKnown, active: enumsKnown ? state >= 1 && state <= 5 : null,
    target_bytes: target, written_bytes: written, read_bytes: read, write_elapsed_ms: writeMs,
    read_elapsed_ms: readMs, mismatch_count: mismatches, error, filesystem_result: fs,
    file_remains: Boolean(flags & 1), volume_mounted: Boolean(flags & 2),
    write_bytes_per_second: writeMs ? written * 1000 / writeMs : null,
    read_bytes_per_second: readMs ? read * 1000 / readMs : null,
    progress_percent: target ? (written + read) * 100 / (2 * target) : 0 };
}

const printable = bytes => bytes.every(byte => byte >= 32 && byte <= 126);
const u32 = (report, offset) => new DataView(report.buffer).getUint32(offset, true);
/** Per-opcode request rules over the full 68-byte report: [3] flags, [4..] arguments; `unused(n)` = zero tail. */
const REQUEST_RULES = {
  [OP.LED]: (r, unused) => r[3] === 0 && r[4] <= 1 && unused(5),
  [OP.REBOOT]: (r, unused) => r[3] === 1 && unused(4),
  [OP.MSC]: (r, unused) => r[4] <= 2 && r[3] === (r[4] === 2 ? 1 : 0) && unused(5),
  [OP.SELF_TEST]: (r, unused) => r[3] === 0 && r[4] <= 1 && unused(9)
    && (r[4] === 0 ? unused(5) : u32(r, 5) >= 1 && u32(r, 5) <= 1024),
  [OP.SAVE_SETTINGS]: (r, unused) => r[3] <= 3 && CLOCKS_MHZ.includes(r[4] | r[5] << 8) && r[6] <= 1 && r[7] <= 7
    && unused(8),
  [OP.STAGE_SSID]: (r, unused) => r[3] === 0 && r[4] <= 32 && unused(5 + r[4]),
  [OP.STAGE_PASSWORD]: (r, unused) => r[3] === 0 && (r[4] === 0 || r[4] >= 8 && r[4] <= 63)
    && printable(r.subarray(5, 5 + r[4])) && unused(5 + r[4]),
  [OP.STAGE_KEY]: (r, unused) => r[3] === 0 && [0, 32].includes(r[4]) && unused(5 + r[4]),
  [OP.TEST_WIFI]: (r, unused) => r[3] === 0 && unused(4),
  [OP.CANCEL_TEST]: (r, unused) => r[3] === 0 && unused(4),
  [OP.SCAN]: (r, unused) => r[3] === 0 && unused(4),
  [OP.SCAN_ENTRY]: (r, unused) => r[3] === 0 && r[4] < 18 && (r[5] | r[6] << 8) !== 0 && unused(7),
  [OP.TCP_CLI_PROOF]: (r, unused) => r[3] === 0 && unused(20),
};

export function buildControlRequest(transaction, opcode, { flags = 0, args = [] } = {}) {
  requireValid(Number.isInteger(transaction) && transaction >= 1 && transaction <= 255, "transaction ID");
  requireValid(Number.isInteger(opcode) && opcode >= 0 && opcode <= 255
    && Number.isInteger(flags) && flags >= 0 && flags <= 255
    && args.length <= 64 && [...args].every(value => Number.isInteger(value) && value >= 0 && value <= 255),
  "command arguments");
  const report = Uint8Array.from([1, transaction, opcode, flags, ...args, ...Array(64 - args.length).fill(0)]);
  requireValid(REQUEST_RULES[opcode]?.(report, offset => zero(report.subarray(offset))), "control request");
  return report;
}

export function decodeControlResult(report, { ble = false } = {}) {
  const view = payload(report, 68);
  const transaction = view.getUint8(1);
  const opcode = view.getUint8(2);
  const status = view.getUint8(3);
  const error = view.getUint8(4);
  const known = opcode >= 1 && opcode <= 13;
  const settingsResult = view.getUint8(8);
  const settings = decodeSettings(bytesAt(view, 5, 4));
  const allowedErrors = [[0], [0], [0], [1, 2, 3], [4, 7], [5], [6]];
  requireValid(allowedErrors[status]?.includes(error)
    && (status === 0 ? transaction === 0 && opcode === 0
      : transaction !== 0 && (known ? error !== 7
        && !([1, 10, 13].includes(opcode) && status === 5) && !([1, 2, 3].includes(opcode) && status === 4)
        && !([6, 7, 8, 10, 12, 13].includes(opcode) && status === 1)
        && !(status === 3 && error === 2 && ![2, 3].includes(opcode))
        && !(opcode === 2 && status === 3 && error === 3) : status === 4 && error === 7))
    , "control result");
  const data = bytesAt(view, 5, 63).slice();
  const detail = [3, 5, 6].includes(status) ? data[0] : 0;
  requireValid((ble || detail !== 28)
    && (!ble || !known || BLE_OPCODES.some(value => value === opcode) || status === 4 && error === 4),
  "transport result");
  const detailRules = {
    1: [5, [2, 3, 4, 5]], 2: [5, [2, 3, 4, 5]], 3: [3, [4]],
    4: [6, [4]], 5: [6, [4]], 6: [6, [4]], 7: [6, [4]], 10: [6, [4]],
    16: [3, [6]], 17: [3, [7]], 18: [3, [8]], 19: [3, [5, 9]], 20: [3, [5, 9]],
    21: [3, [5, 9]], 22: [3, [12]], 23: [3, [12]], 24: [3, [13]],
    25: [5, [5, 6, 7, 8, 9, 11]], 26: [5, [5, 9, 11, 12]], 27: [3, [5]], 28: [5, [2, 5]],
  };
  if ([3, 5, 6].includes(status)) {
    requireValid(zero(data.subarray(1)) && (!detail || detailRules[detail]?.[0] === status
      && detailRules[detail][1].includes(opcode) && (status !== 3 || error === 3)), "result detail");
  } else if (status === 2 && opcode === 5) {
    requireValid(validSettings(settings) && settingsResult <= 7 && data[4] <= 7 && zero(data.subarray(5)),
      "saved settings result");
  } else if (status === 2 && opcode === 12) {
    requireValid((data[0] | data[1] << 8) !== 0 && data[2] < 18 && data[3] <= 32
      && zero(data.subarray(4 + data[3], 36)) && data[37] <= 7 && zero(data.subarray(38)), "scan entry result");
  } else {
    requireValid(zero(data.subarray(status === 2 && opcode === 13 ? 16 : 0)), "result data");
  }
  return { transaction_id: transaction, opcode, status, error_reason: error,
    detail, data, commit_mask: opcode === 5 && status === 2 ? data[4] : 0,
    settings: opcode === 5 && status === 2 ? settings : null };
}

export function matchesPicoForge(device) {
  return device.vendorId === DEVICE_FILTER.vendorId && PRODUCT_IDS.includes(device.productId)
    && device.collections.some(collection => collection.usagePage === DEVICE_FILTER.usagePage
      && collection.usage === DEVICE_FILTER.usage);
}

const bytesOf = report => report instanceof DataView
  ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;

/** Preserve opaque bytes separately from the printable, never round-tripped label. */
export function displaySsid(bytes) {
  let text = "";
  for (let index = 0; index < bytes.length;) {
    const byte = bytes[index];
    const size = byte < 128 ? 1 : byte >= 194 && byte <= 223 ? 2 : byte <= 239 && byte >= 224 ? 3
      : byte >= 240 && byte <= 244 ? 4 : 0;
    let decoded;
    try {
      if (!size || index + size > bytes.length) throw new Error();
      decoded = new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(index, index + size));
    } catch { decoded = null; }
    if (!decoded || /[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]/u.test(decoded)) {
      text += `\\x${byte.toString(16).padStart(2, "0").toUpperCase()}`;
      index++;
    } else { text += decoded; index += size; }
  }
  return text;
}

export function ssidBytes(settings) {
  if (settings.ssid_hex !== undefined) {
    if (typeof settings.ssid_hex !== "string" || !/^(?:[a-f0-9]{2}){0,32}$/i.test(settings.ssid_hex)) {
      throw new Error("SSID hex must encode at most 32 bytes");
    }
    return hexToBytes(settings.ssid_hex);
  }
  if (typeof settings.ssid !== "string") throw new Error("SSID is required");
  const bytes = new TextEncoder().encode(settings.ssid);
  if (bytes.length > 32) throw new Error("SSID must be at most 32 UTF-8 bytes");
  return bytes;
}

export function decodeWireless(report, settings) {
  const bytes = bytesOf(report);
  if (!(bytes instanceof Uint8Array) || bytes.length !== 48 || bytes[0] !== 1 || bytes[1] > 6
    || bytes[3] > 2 || bytes[9] > 32 || !zero(bytes.subarray(10 + bytes[9], 42))
    || bytes[42] > 3 || bytes[45] > 18 || bytes[42] !== 2 && bytes[45] !== 0
    || bytes[42] !== 0 && !(bytes[43] | bytes[44] << 8)
    || bytes[1] >= 4 && !bytes[46]
    || bytes[47] & 8 && bytes[4] & 240
    || bytes[47] & 1 && !(bytes[4] & 16) || bytes[47] & 2 && !(bytes[4] & 32)
    || bytes[47] & 4 && !(bytes[4] & 64)
    || bytes[4] & 128 && (!(bytes[4] & 48) || !bytes[46])
    || bytes[47] & 1 && (!(bytes[47] & 2) || !(bytes[4] & 32))) {
    throw new Error("Invalid wireless status report");
  }
  const ssid = bytes.subarray(10, 10 + bytes[9]);
  return {
    ...(settings ? { wifi_enabled: Boolean(settings.radios & 1), ble_enabled: Boolean(settings.radios & 2),
      tcp_cli_enabled: settings.tcp_cli_enabled } : {}),
    wifi_state: ["disabled", "connecting", "connected", "failed", "testing", "test_joined", "test_failed"][bytes[1]],
    state: bytes[1], detail: bytes[2] <= 6 ? bytes[2] : 0,
    phase: WIRELESS_TEST_DETAILS[bytes[2]] ?? "", ble_state: ["off", "advertising", "connected"][bytes[3]],
    network_configured: Boolean(bytes[4] & 1), ble_key_configured: Boolean(bytes[4] & 2),
    tcp_cli_started: Boolean(bytes[4] & 4),
    staged_mask: bytes[4] >> 4 & 7, pair_tested: Boolean(bytes[4] & 128), staged_state: bytes[47] & 15,
    ssid_staged: Boolean(bytes[4] & 16), password_staged: Boolean(bytes[4] & 32), key_staged: Boolean(bytes[4] & 64),
    ipv4: [...bytes.subarray(5, 9)].join("."), ssid: displaySsid(ssid), ssid_hex: bytesToHex(ssid),
    scan_state: bytes[42], scan_generation: bytes[43] | bytes[44] << 8, scan_count: bytes[45],
    test_generation: bytes[46],
  };
}

export function decodeScanEntry(result) {
  const data = result.data;
  const raw = data.subarray(4, 4 + data[3]);
  const security = data[37];
  return { generation: data[0] | data[1] << 8, index: data[2], ssid: displaySsid(raw), ssid_hex: bytesToHex(raw),
    rssi: new DataView(data.buffer, data.byteOffset).getInt8(36), secured: Boolean(security),
    security: security & 4 ? security & 2 ? "WPA/RSN" : "RSN (WPA2/WPA3)"
      : security & 2 ? "WPA" : security & 1 ? "WEP" : "Open" };
}
