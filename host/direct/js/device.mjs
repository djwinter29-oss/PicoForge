export const DEVICE_FILTER = Object.freeze({
  vendorId: 0xcafe, productId: 0x4011, usagePage: 0xff00, usage: 1,
});
export const IDENTITY_REPORT_ID = 1;
export const CONFIG_REPORT_ID = 2;
export const TELEMETRY_REPORT_ID = 3;
export const STORAGE_REPORT_ID = 10;
export const SELF_TEST_REPORT_ID = 11;
export const CONTROL_REPORT_ID = 20;

function payload(report, size) {
  const view = report instanceof DataView ? report
    : report instanceof Uint8Array ? new DataView(report.buffer, report.byteOffset, report.byteLength) : null;
  // WebHID exposes the ID separately; no value-based framing or padding removal.
  if (!view || view.byteLength !== size) throw new Error(`Expected exactly ${size} HID payload bytes`);
  if (view.getUint8(0) !== 1) throw new Error(`Unsupported HID layout version ${view.getUint8(0)}`);
  return view;
}

function requireValid(condition, label) {
  if (!condition) throw new Error(`Invalid HID ${label}`);
}

const validClock = clock => [0, 150, 200, 250].includes(clock);
// radios: bit 0 Wi-Fi, bit 1 Bluetooth; omitted (0) on boards without a radio.
const validSettings = settings => settings && validClock(settings.system_clock_mhz)
  && [0, 1].includes(settings.sdcard_profile) && [undefined, 0, 1, 2, 3].includes(settings.radios)
  && (settings.tcp_cli_enabled === undefined || typeof settings.tcp_cli_enabled === "boolean");
const bytesAt = (view, offset, size) => new Uint8Array(view.buffer, view.byteOffset + offset, size);
const zero = bytes => bytes.every(byte => byte === 0);
const SAVE_SETTINGS_FORCE_MSC = 1;

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
    serial_number: [...bytesAt(view, 3, 8)].map(byte => byte.toString(16).padStart(2, "0")).join("").toUpperCase(),
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
    radios: flags >> 3 & 3, tcp_cli_enabled: Boolean(flags & 32) };
  const maxGpio = [1, 2, 3, 4].includes(identity.board_model) || identity.mcu_id === 1 ? 29
    : identity.mcu_id === 2 ? 47 : -1;
  requireValid(validSettings(settings) && !(flags & ~63) && (identity.capabilities & 128 || !(flags & 56))
    && [0, 1, 255].includes(uart)
    && (tx === 255) === (rx === 255) && (!(flags & 4) || (uart !== 255 && tx !== 255))
    && Boolean(flags & 1) === (detect !== 255) && Boolean(flags & 2) === (led !== 255)
    && (!(flags & 2) || Boolean(identity.capabilities & 1))
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

export function buildControlRequest(transaction, opcode, { flags = 0, args = [] } = {}) {
  requireValid(Number.isInteger(transaction) && transaction >= 1 && transaction <= 255, "transaction ID");
  requireValid(Number.isInteger(opcode) && opcode >= 0 && opcode <= 255
    && Number.isInteger(flags) && flags >= 0 && flags <= 255
    && args.length <= 64 && [...args].every(value => Number.isInteger(value) && value >= 0 && value <= 255),
  "command arguments");
  const report = Uint8Array.from([1, transaction, opcode, flags, ...args, ...Array(64 - args.length).fill(0)]);
  const arg = report[4];
  const unused = offset => zero(report.subarray(offset));
  requireValid(opcode === 1 ? flags === 0 && arg <= 1 && unused(5)
    : opcode === 2 ? flags === 1 && unused(4)
      : opcode === 3 ? arg <= 2 && flags === (arg === 2 ? 1 : 0) && unused(5)
        : opcode === 4 ? flags === 0 && arg <= 1
          && (arg === 0 ? unused(5) : new DataView(report.buffer).getUint32(5, true) >= 1
            && new DataView(report.buffer).getUint32(5, true) <= 1024) && unused(9)
          : opcode === 5 ? flags <= 3 && validClock(report[4] | report[5] << 8)
            && report[6] <= 1 && report[7] <= 7 && unused(8)
            : [6, 8].includes(opcode) ? flags === 0 && arg <= 32
              && (opcode !== 8 || arg === 0 || arg === 32) && unused(5 + arg)
              : opcode === 7 ? flags === 0 && (arg === 0 || arg >= 8 && arg <= 63)
                && report.subarray(5, 5 + arg).every(byte => byte >= 32 && byte <= 126) && unused(5 + arg)
                : [9, 10, 11].includes(opcode) ? flags === 0 && unused(4)
                  : opcode === 12 ? flags === 0 && arg < 18 && (report[5] | report[6] << 8) !== 0 && unused(7)
                    : opcode === 13 && flags === 0 && unused(20), "control request");
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
  const settings = { system_clock_mhz: view.getUint16(5, true), sdcard_profile: view.getUint8(7),
    radios: settingsResult & 3, tcp_cli_enabled: Boolean(settingsResult & 4) };
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
    && (!ble || !known || [1, 2, 5].includes(opcode) || status === 4 && error === 4), "transport result");
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
  return device.vendorId === DEVICE_FILTER.vendorId && [0x4011, 0x4012].includes(device.productId)
    && device.collections.some(collection => collection.usagePage === DEVICE_FILTER.usagePage
      && collection.usage === DEVICE_FILTER.usage);
}

const EMPTY = {
  hardware: null, settings: null, settings_supported: false, settings_message: null,
  runtime: null, board_status: null, memory: null, filesystem_format: null, msc: null, self_test: null,
  wireless: null, wireless_error: null, recovery_message: null,
  freshness: { 3: "unavailable", 10: "unavailable", 11: "unavailable" },
  self_test_command_pending: false, msc_command_pending: false, reboot_pending: false,
};
const sameSettings = (a, b) => a.system_clock_mhz === b.system_clock_mhz && a.sdcard_profile === b.sdcard_profile
  && (a.radios ?? 0) === (b.radios ?? 0) && a.tcp_cli_enabled === b.tcp_cli_enabled;
const PENDING_OPERATION_KEY = "picoforge.direct.pending-operation";
const isUid = value => typeof value === "string" && /^[A-F0-9]{16}$/i.test(value);
const validBoardSettings = value => value && [0, 150, 200, 250].includes(value.system_clock_mhz)
  && [0, 1].includes(value.sdcard_profile) && [0, 1, 2, 3].includes(value.radios)
  && (value.tcp_cli_enabled === undefined || typeof value.tcp_cli_enabled === "boolean");
const hasExactKeys = (value, keys) => value && typeof value === "object" && !Array.isArray(value)
  && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
function validatePendingOperation(record) {
  if (!record || record.version !== 2 || !isUid(record.uid)) throw new Error("Invalid saved operation identity");
  if (record.kind === "control") {
    const request = record.request;
    const requestKeys = request?.opcode === 5 ? ["transaction_id", "opcode", "settings", "commit_mask"]
      : request?.opcode === 12 ? ["transaction_id", "opcode", "scan_generation", "scan_index"]
        : request?.lifecycle ? ["transaction_id", "opcode", "lifecycle"] : ["transaction_id", "opcode"];
    const settingsKeys = ["system_clock_mhz", "sdcard_profile", "radios", "tcp_cli_enabled"];
    if (!hasExactKeys(record, ["version", "kind", "uid", "request"])
      || !hasExactKeys(request, requestKeys) || !Number.isInteger(request.transaction_id) || request.transaction_id < 1
      || request.transaction_id > 255 || !Number.isInteger(request.opcode) || request.opcode < 1 || request.opcode > 13
      || request.lifecycle && (!hasExactKeys(request.lifecycle, ["operation", "generation"])
        || request.lifecycle.operation !== request.opcode || ![9, 11].includes(request.opcode)
        || !Number.isInteger(request.lifecycle.generation) || request.lifecycle.generation < 1
        || request.lifecycle.generation > (request.opcode === 9 ? 255 : 65535))
      || request.opcode === 5 && (!hasExactKeys(request.settings, settingsKeys)
        || !validBoardSettings(request.settings) || !Number.isInteger(request.commit_mask)
        || request.commit_mask < 0 || request.commit_mask > 7)
      || request.opcode === 12 && (!Number.isInteger(request.scan_generation)
        || request.scan_generation < 1 || request.scan_generation > 65535
        || !Number.isInteger(request.scan_index) || request.scan_index < 0 || request.scan_index >= 18)) {
      throw new Error("Invalid saved control transaction");
    }
  } else if (record.kind === "wireless-lifecycle") {
    if (!hasExactKeys(record, ["version", "kind", "uid", "operation", "generation"])
      || ![9, 11].includes(record.operation) || !Number.isInteger(record.generation)
      || record.generation < 1 || record.generation > (record.operation === 9 ? 255 : 65535)) {
      throw new Error("Invalid saved wireless lifecycle");
    }
  } else throw new Error("Unknown saved operation kind");
  return record;
}
const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

export async function withTimeout(operation, milliseconds) {
  let timer;
  try {
    return await Promise.race([operation, new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new DOMException("HID I/O timeout; command outcome uncertain", "TimeoutError")),
        milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

export class PicoForgeWebHid {
  constructor(hid, onChange = () => {}, { now = () => performance.now(), sleep = wait, timeout = 3000,
    pendingStore } = {}) {
    if (pendingStore === undefined) {
      try { pendingStore = globalThis.sessionStorage; }
      catch { pendingStore = null; }
    }
    Object.assign(this, { hid, onChange, now, sleep, timeout, device: null, reconnectUid: null,
      transaction: 0, outstanding: null, pendingWirelessCommand: null, reconcileWirelessCommand: null,
      wirelessOperation: null,
      receipts: new Map(), expiryTimer: null, generation: 0, mscPendingEnabled: null,
      selfTestPendingSequence: null, pendingStore, recoveryError: null });
    try {
      if (!pendingStore) throw new Error("session storage is unavailable");
      const raw = pendingStore?.getItem(PENDING_OPERATION_KEY);
      if (raw) {
        const record = validatePendingOperation(JSON.parse(raw));
        this.transaction = record.request?.transaction_id ?? 0;
        this.reconnectUid = record.uid;
        if (record.kind === "control") this.outstanding = record.request;
        if (record.kind === "wireless-lifecycle") this.pendingWirelessCommand = record;
      }
    } catch (error) {
      this.recoveryError = `Pending operation recovery data is unavailable or invalid: ${error.message}`;
    }
    this.state = { ...EMPTY, connected: false, reconnecting: false, busy: false,
      outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
      error: this.recoveryError ?? (hid ? null : "WebHID is unavailable; use a supported browser on localhost or HTTPS") };
    this.onInputReport = event => {
      if (event.device !== this.device || !this.state.connected) return;
      const id = event.reportId;
      if (![3, 10, 11].includes(id) || (id === 11 && !(this.state.hardware.capabilities & 32))) return;
      try {
        const changes = id === 3 ? { runtime: decodeTelemetryReport(event.data, this.state.hardware) }
          : id === 10 ? decodeStorageReport(event.data, this.state.hardware)
            : { self_test: decodeSelfTestReport(event.data) };
        if (id === 3) changes.board_status = changes.runtime;
        if (id === 10 && !this.state.busy && !this.outstanding && this.mscPendingEnabled !== null
          && (changes.msc.request_rejected ||
            (changes.msc.enabled === this.mscPendingEnabled && !changes.msc.transition_pending))) {
          this.mscPendingEnabled = null;
          changes.msc_command_pending = false;
        }
        if (id === 11 && !this.state.busy && !this.outstanding
          && changes.self_test.sequence !== this.selfTestPendingSequence) {
          changes.self_test_command_pending = false;
        }
        this.receipts.set(id, this.now());
        this.scheduleExpiry();
        this.update({ ...changes, freshness: { ...this.state.freshness, [id]: "fresh" } });
      } catch (error) { this.update({ error: error.message }); }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      this.clearConnection();
      this.update({ reconnecting: Boolean(this.reconnectUid), error: this.recoveryError ?? (this.outstanding
        ? "Command outcome uncertain after disconnect; no request will be replayed."
        : this.pendingWirelessCommand
          ? "Wireless command outcome uncertain; reconnect to inspect its result." : null) });
    };
    this.onConnect = event => {
      if (this.reconnectUid && matchesPicoForge(event.device) && !this.device) void this.attachDevice(event.device);
    };
    this.onSuspend = event => { if (event.device === this.device) this.expireReports(true); };
    hid?.addEventListener("disconnect", this.onDisconnect);
    hid?.addEventListener("connect", this.onConnect);
    hid?.addEventListener("suspend", this.onSuspend);
  }

  update(changes) {
    this.state = { ...this.state, ...changes };
    this.onChange(this.state);
  }

  persistPendingOperation() {
    if (!this.pendingStore) throw new Error("Session storage is unavailable; refusing an untracked device write.");
    const uid = this.state.hardware?.serial_number ?? this.reconnectUid;
    const record = this.outstanding ? { version: 2, kind: "control", uid, request: this.outstanding }
      : this.pendingWirelessCommand ? { version: 2, kind: "wireless-lifecycle", uid, ...this.pendingWirelessCommand }
        : null;
    if (record) this.pendingStore.setItem(PENDING_OPERATION_KEY, JSON.stringify(validatePendingOperation(record)));
    else this.clearPersistedOperation();
  }

  clearPersistedOperation() {
    if (!this.pendingStore) throw new Error("Session storage is unavailable; cannot clear pending operation.");
    this.pendingStore.removeItem(PENDING_OPERATION_KEY);
  }

  expireReports(suspended = false) {
    const changes = {};
    const freshness = { ...this.state.freshness };
    for (const [id, received] of this.receipts) {
      if (!suspended && this.now() - received < this.reportLifetime(id)) continue;
      this.receipts.delete(id);
      freshness[id] = "stale";
      Object.assign(changes, id === 3 ? { runtime: null, board_status: null }
        : id === 10 ? { memory: null, filesystem_format: null, msc: null }
          : id === 12 ? { wireless: null, wireless_error: "Wireless status is stale" } : { self_test: null });
    }
    if (Object.keys(changes).length) this.update({ ...changes, freshness });
    this.scheduleExpiry();
  }

  scheduleExpiry() {
    clearTimeout(this.expiryTimer);
    this.expiryTimer = null;
    if (!this.device || !this.receipts.size) return;
    const deadline = Math.min(...[...this.receipts].map(([id, received]) =>
      received + this.reportLifetime(id)));
    this.expiryTimer = setTimeout(() => {
      this.expiryTimer = null;
      this.expireReports();
    }, Math.max(0, deadline - this.now()));
    this.expiryTimer.unref?.();
  }

  reportLifetime(id) { return id === 12 ? 3 * (this.wirelessInterval ?? 2000) : id === 3 ? 750 : 1500; }

  clearConnection() {
    this.generation++;
    this.device?.removeEventListener("inputreport", this.onInputReport);
    clearTimeout(this.expiryTimer);
    this.expiryTimer = null;
    this.device = null;
    this.savedSsidHex = null;
    this.receipts.clear();
    this.update({ ...EMPTY, connected: false, busy: false,
      outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
      msc_command_pending: this.mscPendingEnabled !== null });
  }

  async connect() {
    if (!this.hid || this.state.busy || this.device || this.state.reconnecting) return;
    this.update({ busy: true, error: this.recoveryError });
    try {
      const devices = await this.hid.requestDevice({
        filters: [0x4011, 0x4012].map(productId => ({ ...DEVICE_FILTER, productId })),
      });
      if (!devices.length) return;
      requireValid(devices.length === 1 && matchesPicoForge(devices[0]), "device selection");
      await this.attachDevice(devices[0]);
    } catch (error) { this.update({ error: error.message }); }
    finally { this.update({ busy: false }); }
  }

  async attachDevice(device) {
    const generation = this.generation;
    this.update({ busy: true });
    try {
      if (!device.opened) await device.open();
      if (generation !== this.generation) throw new Error("Connection cancelled");
      this.device = device;
      const hardware = decodeIdentityReport(await withTimeout(device.receiveFeatureReport(1), this.timeout));
      const expectedUid = this.reconnectUid;
      if (expectedUid && hardware.serial_number !== expectedUid) {
        await device.close();
        if (generation === this.generation && device === this.device) {
          this.device = null;
          this.update({ busy: false, reconnecting: true });
        }
        return;
      }
      const settings = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), this.timeout), hardware);
      if (generation !== this.generation || device !== this.device) throw new Error("Connection cancelled");
      this.reconnectUid = hardware.serial_number;
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ ...EMPTY, connected: true, reconnecting: false, busy: true,
        outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
        error: this.recoveryError,
        hardware, settings, settings_supported: Boolean(hardware.capabilities & 4),
        msc_command_pending: this.mscPendingEnabled !== null });
      this.scheduleExpiry();
      if (this.outstanding) {
        try {
          const result = await this.pollResult(device, this.outstanding);
          if (result.status === 2 && this.outstanding.settings &&
            (!sameSettings(result.settings, this.outstanding.settings)
              || result.commit_mask !== this.outstanding.commit_mask)) {
            throw new Error("Firmware did not confirm the selected values after reconnect");
          }
          const lifecycle = result.status === 2 ? this.outstanding.lifecycle : null;
          this.reconnectedResult = result;
          this.outstanding = null;
          if (lifecycle) {
            this.pendingWirelessCommand = lifecycle;
            this.persistPendingOperation();
          } else this.clearPersistedOperation();
          if (result.opcode === 3 && result.status !== 2) {
            this.mscPendingEnabled = null;
            this.update({ msc_command_pending: false });
          }
          this.update({ outcome_uncertain: false, error: result.status === 2 ? null
            : `Command ${result.opcode}: status ${result.status}, error ${result.error_reason}`,
          recovery_message: result.status === 2 ? result.opcode === 10
            ? "Wireless test cancellation confirmed after reconnect."
            : "Pending command result confirmed after reconnect." : null });
        } catch (error) {
          this.reconnectError = error;
          this.update({ outcome_uncertain: true, error: error.message });
        }
      }
      if (this.pendingWirelessCommand && !this.outstanding) {
        const pending = this.pendingWirelessCommand;
        try {
          if (!this.reconcileWirelessCommand) {
            throw new Error("Wireless command remains uncertain; reconnect and inspect its result.");
          }
          const outcome = await this.reconcileWirelessCommand(device, pending);
          if (generation !== this.generation || device !== this.device) {
            throw new Error("Wireless command outcome uncertain after disconnect");
          }
          if (!outcome) throw new Error("Wireless command remains uncertain; the matching result is not available.");
          this.clearPersistedOperation();
          pending.outcome = outcome;
          this.pendingWirelessCommand = null;
          this.update({ outcome_uncertain: Boolean(this.recoveryError),
            error: this.recoveryError ?? (outcome.kind === "failed" ? outcome.message : null),
            recovery_message: outcome.kind === "completed" ? "Wireless operation confirmed after reconnect." : null });
        } catch (error) {
          this.update({ outcome_uncertain: true, error: error.message });
        }
      }
      if (generation === this.generation && device === this.device) this.update({ busy: false });
    } catch (error) {
      if (device.opened) { try { await device.close(); } catch {} }
      if (generation === this.generation) {
        this.clearConnection();
        this.update({ reconnecting: false, error: error.message });
      }
    }
  }

  async disconnect() {
    if (this.state.busy) return;
    const device = this.device;
    if (!this.outstanding && !this.pendingWirelessCommand) this.reconnectUid = null;
    this.mscPendingEnabled = null;
    this.clearConnection();
    this.update({ reconnecting: false, error: this.recoveryError });
    if (device?.opened) {
      try { await device.close(); } catch (error) { this.update({ error: error.message }); }
    }
  }

  storageReady() {
    this.expireReports();
    return this.state.freshness[10] === "fresh" && this.state.msc && !this.state.msc.transition_pending
      && !this.state.msc_command_pending && !this.state.self_test_command_pending
      && (!(this.state.hardware?.capabilities & 32)
        || (this.state.freshness[11] === "fresh" && this.state.self_test?.valid_for_control));
  }

  wirelessReady() {
    return this.state.connected && this.state.transport !== "ble" && this.storageReady()
      && !this.state.msc?.enabled && !this.state.msc?.transition_pending && !this.state.self_test?.active
      && !this.state.busy && !this.state.outcome_uncertain && !this.state.reboot_pending
      && !this.outstanding && !this.pendingWirelessCommand && !this.wirelessOperation;
  }

  async pollResult(device, request) {
    const deadline = this.now() + this.timeout;
    const generation = this.generation;
    for (let attempt = 0; attempt < Math.ceil(this.timeout / 50) && this.now() < deadline; attempt++) {
      if (device !== this.device) throw new Error("Command outcome uncertain after disconnect");
      const report = await withTimeout(device.receiveFeatureReport(20), deadline - this.now());
      if (device !== this.device || generation !== this.generation) {
        throw new Error("Command outcome uncertain after disconnect");
      }
      const result = decodeControlResult(report, { ble: Boolean(this.bleTransport) });
      if (result.transaction_id === request.transaction_id && result.opcode === request.opcode && result.status > 1) {
        if (result.status === 2 && request.opcode === 12
          && ((result.data[0] | result.data[1] << 8) !== request.scan_generation
            || result.data[2] !== request.scan_index)) {
          throw new Error("Scan entry did not match the requested generation and index");
        }
        return result;
      }
      await this.sleep(50);
    }
    throw new Error("Command outcome uncertain: result timeout; disconnect/reconnect to read it, never replay");
  }

  async tcpCliAuthProof(nonce, uid) {
    const hardware = this.state.hardware;
    if (!this.device || !this.state.connected || !(hardware?.capabilities & 128)) {
      throw new Error("Connect the matching PicoForge board over USB to authorize its Wi-Fi console");
    }
    if (!(nonce instanceof Uint8Array) || nonce.length !== 16
      || !(uid instanceof Uint8Array) || uid.length !== 8) {
      throw new Error("Invalid TCP CLI authentication challenge");
    }
    const expectedUid = Uint8Array.from(hardware.serial_number.match(/../g), byte => parseInt(byte, 16));
    if (uid.some((byte, index) => byte !== expectedUid[index])) {
      throw new Error("The Wi-Fi console challenge belongs to a different board");
    }
    const result = await this.command(13, { args: nonce });
    return result.data.slice(0, 16);
  }

  async command(opcode, options) {
    this.expireReports();
    if (!this.device || !this.state.connected || this.state.busy || this.state.outcome_uncertain
      || this.outstanding || this.state.self_test_command_pending || this.state.msc_command_pending
      || this.state.reboot_pending || this.pendingWirelessCommand
      || this.wirelessOperation && options?.workflow !== this.wirelessOperation) {
      throw new Error("Command unavailable or previous outcome uncertain");
    }
    const bit = opcode >= 6 ? 128 : ({ 1: 1, 2: 2, 4: 32, 5: 4 })[opcode];
    if (bit && !(this.state.hardware.capabilities & bit)) throw new Error("Unavailable capability");
    const transaction = this.transaction % 255 + 1;
    const report = buildControlRequest(transaction, opcode, options);
    const forcedSave = opcode === 5 && Boolean(report[3] & SAVE_SETTINGS_FORCE_MSC);
    this.reconnectedResult = null;
    this.reconnectError = null;
    this.transaction = transaction;
    const request = { transaction_id: transaction, opcode };
    if (opcode === 12) {
      request.scan_index = report[4];
      request.scan_generation = report[5] | report[6] << 8;
    }
    if (options?.lifecycle) request.lifecycle = options.lifecycle;
    const settingsToSave = opcode === 5 ? { system_clock_mhz: report[4] | report[5] << 8,
      sdcard_profile: report[6], radios: report[7] & 3, tcp_cli_enabled: Boolean(report[7] & 4) } : null;
    if (settingsToSave) {
      request.settings = settingsToSave;
      request.commit_mask = options?.commitMask ?? 0;
    }
    const device = this.device;
    const generation = this.generation;
    this.outstanding = request;
    if (opcode === 3) this.mscPendingEnabled = report[4] === 1;
    try { this.persistPendingOperation(); }
    catch (error) {
      this.outstanding = null;
      this.mscPendingEnabled = null;
      throw error;
    }
    this.update({ busy: true, error: null, recovery_message: null,
      ...(opcode === 3 ? { msc_command_pending: true } : {}) });
    let sent = false;
    try {
      await withTimeout(device.sendFeatureReport(20, report), this.timeout);
      sent = true;
      if (generation !== this.generation) throw new Error("Command outcome uncertain after disconnect");
      const result = await this.pollResult(device, request);
      if (result.status !== 2) {
        this.clearPersistedOperation();
        this.outstanding = null;
        if (opcode === 3) {
          this.mscPendingEnabled = null;
          this.update({ msc_command_pending: false });
        }
        throw new Error(`Command ${opcode}: status ${result.status}, error ${result.error_reason}, `
          + `detail ${result.detail}`);
      }
      if (opcode === 5) {
        const saved = result.settings;
        if (!sameSettings(saved, settingsToSave) || result.commit_mask !== request.commit_mask) {
          this.update({ outcome_uncertain: true });
          throw new Error("Firmware did not confirm the selected values");
        }
        this.update({ settings: saved });
      }
      if (request.lifecycle) {
        this.pendingWirelessCommand = request.lifecycle;
        this.outstanding = null;
        this.persistPendingOperation();
      } else {
        this.clearPersistedOperation();
        this.outstanding = null;
      }
      if (opcode === 3) this.update({ msc_command_pending: true });
      if (opcode === 4) {
        this.selfTestPendingSequence = this.state.self_test?.sequence ?? null;
        this.update({ self_test_command_pending: true });
      }
      if (opcode === 2 || opcode === 5) this.update({ reboot_pending: true });
      return result;
    } catch (error) {
      if (forcedSave && sent && this.reconnectUid &&
        !(generation === this.generation && this.state.outcome_uncertain) &&
        (this.outstanding || this.reconnectedResult || this.reconnectError)) {
        const deadline = this.now() + 5000;
        while (this.now() < deadline && !this.reconnectedResult && this.outstanding && !this.reconnectError) {
          await this.sleep(50);
        }
        const result = this.reconnectedResult;
        if (result?.transaction_id === transaction && result.opcode === opcode && result.status !== 2) {
          throw new Error(`Command ${opcode}: status ${result.status}, error ${result.error_reason}`);
        }
        if (result?.transaction_id === transaction && result.opcode === opcode && result.status === 2
          && sameSettings(result.settings, settingsToSave) && result.commit_mask === request.commit_mask) {
          this.update({ settings: result.settings, reboot_pending: true, outcome_uncertain: false });
          return result;
        }
        if (this.state.error) throw new Error(this.state.error);
      }
      // SET rejection is authoritative: do not poll or accept a prior result.
      if (!sent && error.name !== "TimeoutError" && generation === this.generation) {
        try {
          this.clearPersistedOperation();
          this.outstanding = null;
          if (opcode === 3) {
            this.mscPendingEnabled = null;
            this.update({ msc_command_pending: false });
          }
        } catch (storageError) {
          this.update({ error: `Command was not sent, but recovery state could not be cleared: ${storageError.message}` });
        }
      }
      if (generation === this.generation) {
        this.update({ outcome_uncertain: Boolean(this.outstanding || this.pendingWirelessCommand),
          error: error.message });
      }
      throw error;
    } finally {
      report.fill(0);
      if (generation === this.generation) this.update({ busy: false });
    }
  }

  async setLed(on) {
    requireValid(typeof on === "boolean", "LED state");
    this.expireReports();
    if (!this.state.board_status?.led_available) throw new Error("Fresh LED telemetry unavailable");
    return this.command(1, { args: [Number(on)] });
  }

  async reboot(confirmed = false) {
    if (confirmed !== true) throw new Error("Reboot requires explicit confirmation");
    if (!this.storageReady() || this.state.msc.enabled || this.state.self_test?.active) {
      throw new Error("Fresh idle firmware-owned storage required");
    }
    return this.command(2, { flags: 1 });
  }

  async setMsc(enable, force = false) {
    requireValid(typeof enable === "boolean" && typeof force === "boolean" && !(force && enable), "MSC action");
    if (!this.storageReady() || this.state.self_test?.active
      || (force && !this.state.msc.enabled)) throw new Error("Fresh idle storage status required");
    return this.command(3, { flags: Number(force), args: [force ? 2 : Number(enable)] });
  }

  async setSelfTest(action, sizeMiB = 32) {
    requireValid(["start", "cancel"].includes(action), "self-test action");
    if (!this.storageReady() || this.state.msc.enabled || !this.state.self_test?.valid_for_control
      || (action === "start" && (this.state.self_test.active || !this.state.self_test.volume_mounted
        || !this.state.memory?.initialized))) throw new Error("Fresh self-test and firmware-owned storage required");
    if (action === "cancel") sizeMiB = 0;
    requireValid(Number.isInteger(sizeMiB) && sizeMiB >= (action === "start" ? 1 : 0) && sizeMiB <= 1024,
      "self-test size");
    const size = new Uint8Array(4);
    new DataView(size.buffer).setUint32(0, sizeMiB, true);
    return this.command(4, { args: [Number(action === "start"), ...size] });
  }

  async saveSettings(settings, force = false, tcpCliEnabled) {
    requireValid(validSettings(settings), "boot settings");
    requireValid(typeof force === "boolean", "forced save confirmation");
    requireValid(tcpCliEnabled === undefined || typeof tcpCliEnabled === "boolean", "TCP CLI setting");
    if (!this.storageReady() || this.state.self_test?.active ||
      (this.state.msc.enabled && !force) || (force && !this.state.msc.enabled)) {
      this.update({ settings_message: "Fresh idle firmware-owned storage required before saving." });
      return false;
    }
    try {
      const args = [settings.system_clock_mhz & 255, settings.system_clock_mhz >> 8,
        settings.sdcard_profile, (settings.radios ?? 0)
          | Number(tcpCliEnabled ?? settings.tcp_cli_enabled ?? this.state.settings?.tcp_cli_enabled ?? false) << 2];
      await this.command(5, { flags: Number(force), args });
      this.update({ settings_message: "Settings saved. Waiting for the board to reboot." });
      return true;
    } catch (error) {
      this.update({ settings_message: `Save failed: ${error.message}` });
      return false;
    }
  }
}
