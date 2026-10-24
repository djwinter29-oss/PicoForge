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
const validSettings = settings => settings && validClock(settings.system_clock_mhz)
  && [0, 1].includes(settings.sdcard_profile);
const bytesAt = (view, offset, size) => new Uint8Array(view.buffer, view.byteOffset + offset, size);
const zero = bytes => bytes.every(byte => byte === 0);

export function decodeIdentityReport(report) {
  const view = payload(report, 26);
  const product = view.getUint16(1, true);
  if (product !== 2) throw new Error(`Unknown product (0x${product.toString(16).padStart(4, "0")})`);
  const mcu = view.getUint8(14);
  const board = view.getUint8(15);
  const capabilities = view.getUint8(17);
  const total = view.getUint32(18, true);
  const reserved = view.getUint32(22, true);
  requireValid(!(capabilities & 0xc0) && [8, 16].includes(capabilities & 24)
    && total > 0 && reserved <= total
    && (board !== 1 || mcu === 1) && (![2, 3].includes(board) || mcu === 2), "identity");
  return {
    version: 1, product_id: product, product_name: "PicoForge",
    serial_number: [...bytesAt(view, 3, 8)].map(byte => byte.toString(16).padStart(2, "0")).join("").toUpperCase(),
    firmware_version: [11, 12, 13].map(offset => view.getUint8(offset)).join("."),
    mcu_id: mcu, mcu: ({ 1: "RP2040", 2: "RP2350" })[mcu] ?? `Unknown MCU (${mcu})`,
    board_model: board, board_name: ({ 0: "Unspecified", 1: "Pico", 2: "Pico 2", 3: "Pico 2 W" })[board]
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
  const settings = { system_clock_mhz: view.getUint16(1, true), sdcard_profile: view.getUint8(3) };
  const maxGpio = [1, 2, 3].includes(identity.board_model) || identity.mcu_id === 1 ? 29
    : identity.mcu_id === 2 ? 47 : -1;
  requireValid(validSettings(settings) && !(flags & ~7) && [0, 1, 255].includes(uart)
    && (tx === 255) === (rx === 255) && (!(flags & 4) || (uart !== 255 && tx !== 255))
    && Boolean(flags & 1) === (detect !== 255) && Boolean(flags & 2) === (led !== 255)
    && Boolean(identity.capabilities & 1) === (led !== 255)
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
  const request = view.getUint8(30);
  // Unknown enums remain diagnostic data but can never authorize a state transition.
  const enumsKnown = state <= 8 && error <= 11 && request <= 11 && fs <= 19;
  requireValid(!(flags & ~3) && view.getUint8(31) === 0 && target <= 1073741824
    && written <= target && read <= written && mismatches <= read
    && error !== 3 && request !== 3 && ![9, 10].includes(request)
    && (error > 11 || error === 0 || [5, 6, 8, 9, 10].includes(error))
    && (state > 8 || error > 11 || (state === 7 ? error !== 0 : error === 0))
    && (state > 8 || fs > 19 || (state === 7 && [8, 10].includes(error) ? error !== 8 || fs > 0 : fs === 0))
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
    read_elapsed_ms: readMs, mismatch_count: mismatches, error, filesystem_result: fs, request_error: request,
    file_remains: Boolean(flags & 1), volume_mounted: Boolean(flags & 2),
    write_bytes_per_second: writeMs ? written * 1000 / writeMs : null,
    read_bytes_per_second: readMs ? read * 1000 / readMs : null,
    progress_percent: target ? (written + read) * 100 / (2 * target) : 0 };
}

export function buildControlRequest(transaction, opcode, { flags = 0, args = [] } = {}) {
  requireValid(Number.isInteger(transaction) && transaction >= 1 && transaction <= 255, "transaction ID");
  requireValid(Number.isInteger(opcode) && opcode >= 0 && opcode <= 255
    && Number.isInteger(flags) && flags >= 0 && flags <= 255
    && args.length <= 5 && [...args].every(value => Number.isInteger(value) && value >= 0 && value <= 255),
  "command arguments");
  const report = Uint8Array.from([1, transaction, opcode, flags, ...args, ...Array(5 - args.length).fill(0)]);
  const arg = report[4];
  const unused = offset => zero(report.subarray(offset));
  requireValid(opcode === 1 ? flags === 0 && arg <= 1 && unused(5)
    : opcode === 2 ? flags === 1 && unused(4)
      : opcode === 3 ? arg <= 2 && flags === (arg === 2 ? 1 : 0) && unused(5)
        : opcode === 4 ? flags === 0 && arg <= 1
          && (arg === 0 ? unused(5) : new DataView(report.buffer).getUint32(5, true) >= 1
            && new DataView(report.buffer).getUint32(5, true) <= 1024)
          : opcode === 6 && flags === 0 && validClock(report[4] | report[5] << 8)
            && report[6] <= 1 && unused(7), "control request");
  return report;
}

export function decodeControlResult(report) {
  const view = payload(report, 9);
  const transaction = view.getUint8(1);
  const opcode = view.getUint8(2);
  const status = view.getUint8(3);
  const error = view.getUint8(4);
  const known = [1, 2, 3, 4, 6].includes(opcode);
  const settings = { system_clock_mhz: view.getUint16(5, true), sdcard_profile: view.getUint8(7) };
  const allowedErrors = [[0], [0], [0], [1, 2, 3], [4, 7], [5], [6]];
  requireValid(allowedErrors[status]?.includes(error)
    && (status === 0 ? transaction === 0 && opcode === 0
      : transaction !== 0 && (known ? error !== 7
        && !(opcode === 1 && status === 5) && !(opcode === 3 && status === 4)
        && !(status === 3 && error === 2 && ![2, 3].includes(opcode))
        && !(opcode === 2 && status === 3 && error === 3) : status === 4 && error === 7))
    && (opcode === 6 && status === 2
      ? validSettings(settings) && view.getUint8(8) === 0 : zero(bytesAt(view, 5, 4))), "control result");
  return { transaction_id: transaction, opcode, status, error_reason: error,
    settings: opcode === 6 && status === 2 ? settings : null };
}

export function matchesPicoForge(device) {
  return device.vendorId === DEVICE_FILTER.vendorId && [0x4011, 0x4012].includes(device.productId)
    && device.collections.some(collection => collection.usagePage === DEVICE_FILTER.usagePage
      && collection.usage === DEVICE_FILTER.usage);
}

const EMPTY = {
  hardware: null, settings: null, settings_supported: false, settings_message: null,
  runtime: null, board_status: null, memory: null, filesystem_format: null, msc: null, self_test: null,
  freshness: { 3: "unavailable", 10: "unavailable", 11: "unavailable" },
  self_test_command_pending: false, msc_command_pending: false, reboot_pending: false,
};
const sameSettings = (a, b) => a.system_clock_mhz === b.system_clock_mhz && a.sdcard_profile === b.sdcard_profile;
const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function withTimeout(operation, milliseconds) {
  let timer;
  try {
    return await Promise.race([operation, new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new DOMException("HID I/O timeout; command outcome uncertain", "TimeoutError")),
        milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

export class PicoForgeWebHid {
  constructor(hid, onChange = () => {}, { now = () => performance.now(), sleep = wait, timeout = 3000 } = {}) {
    Object.assign(this, { hid, onChange, now, sleep, timeout, device: null, reconnectSerial: null,
      transaction: 0, outstanding: null, receipts: new Map(), timer: null, generation: 0 });
    this.state = { ...EMPTY, connected: false, reconnecting: false, busy: false, outcome_uncertain: false,
      error: hid ? null : "WebHID is unavailable; use a supported browser on localhost or HTTPS" };
    this.onInputReport = event => {
      if (event.device !== this.device || !this.state.connected) return;
      const id = event.reportId;
      if (![3, 10, 11].includes(id) || (id === 11 && !(this.state.hardware.capabilities & 32))) return;
      try {
        const changes = id === 3 ? { runtime: decodeTelemetryReport(event.data, this.state.hardware) }
          : id === 10 ? decodeStorageReport(event.data, this.state.hardware)
            : { self_test: decodeSelfTestReport(event.data) };
        if (id === 3) changes.board_status = changes.runtime;
        if (id === 10 && !this.state.busy) changes.msc_command_pending = false;
        if (id === 11 && !this.state.busy) changes.self_test_command_pending = false;
        this.receipts.set(id, this.now());
        this.update({ ...changes, freshness: { ...this.state.freshness, [id]: "fresh" } });
      } catch (error) { this.update({ error: error.message }); }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      this.clearConnection();
      this.update({ reconnecting: Boolean(this.reconnectSerial), error: this.outstanding
        ? "Command outcome uncertain after disconnect; no request will be replayed." : null });
    };
    this.onConnect = event => {
      if (this.reconnectSerial && event.device.serialNumber === this.reconnectSerial
        && matchesPicoForge(event.device) && !this.device) void this.attachDevice(event.device);
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

  expireReports(suspended = false) {
    const changes = {};
    const freshness = { ...this.state.freshness };
    for (const [id, received] of this.receipts) {
      if (!suspended && this.now() - received < (id === 3 ? 750 : 1500)) continue;
      freshness[id] = "stale";
      Object.assign(changes, id === 3 ? { runtime: null, board_status: null }
        : id === 10 ? { memory: null, filesystem_format: null, msc: null } : { self_test: null });
    }
    if (Object.keys(changes).length) this.update({ ...changes, freshness });
  }

  clearConnection() {
    this.generation++;
    this.device?.removeEventListener("inputreport", this.onInputReport);
    clearInterval(this.timer);
    this.timer = null;
    this.device = null;
    this.receipts.clear();
    this.update({ ...EMPTY, connected: false, busy: false, outcome_uncertain: Boolean(this.outstanding) });
  }

  async connect() {
    if (!this.hid || this.state.busy || this.device || this.state.reconnecting) return;
    this.update({ busy: true, error: null });
    try {
      const allowed = (await this.hid.getDevices()).filter(matchesPicoForge);
      const devices = allowed.length === 1 ? allowed : await this.hid.requestDevice({
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
      const settings = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), this.timeout), hardware);
      if (generation !== this.generation || device !== this.device) throw new Error("Connection cancelled");
      this.reconnectSerial = device.serialNumber || hardware.serial_number;
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ ...EMPTY, connected: true, reconnecting: false, busy: false, error: null,
        hardware, settings, settings_supported: Boolean(hardware.capabilities & 4) });
      this.timer = setInterval(() => this.expireReports(), 100);
      this.timer.unref?.();
      if (this.outstanding) {
        try {
          const result = await this.pollResult(device, this.outstanding);
          this.outstanding = null;
          this.update({ outcome_uncertain: false, error: result.status === 2 ? null
            : `Command ${result.opcode}: status ${result.status}, error ${result.error_reason}` });
        } catch (error) { this.update({ outcome_uncertain: true, error: error.message }); }
      }
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
    this.reconnectSerial = null;
    this.clearConnection();
    this.update({ reconnecting: false, error: null });
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

  async pollResult(device, request) {
    const deadline = this.now() + this.timeout;
    const generation = this.generation;
    for (let attempt = 0; attempt < Math.ceil(this.timeout / 50) && this.now() < deadline; attempt++) {
      if (device !== this.device) throw new Error("Command outcome uncertain after disconnect");
      const report = await withTimeout(device.receiveFeatureReport(20), deadline - this.now());
      if (device !== this.device || generation !== this.generation) {
        throw new Error("Command outcome uncertain after disconnect");
      }
      const result = decodeControlResult(report);
      if (result.transaction_id === request.transaction_id && result.opcode === request.opcode && result.status > 1) {
        return result;
      }
      await this.sleep(50);
    }
    throw new Error("Command outcome uncertain: result timeout; disconnect/reconnect to read it, never replay");
  }

  async command(opcode, options) {
    this.expireReports();
    if (!this.device || !this.state.connected || this.state.busy || this.state.outcome_uncertain
      || this.outstanding || this.state.self_test_command_pending || this.state.msc_command_pending
      || this.state.reboot_pending) {
      throw new Error("Command unavailable or previous outcome uncertain");
    }
    const bit = ({ 1: 1, 2: 2, 4: 32, 6: 4 })[opcode];
    if (bit && !(this.state.hardware.capabilities & bit)) throw new Error("Unavailable capability");
    const transaction = this.transaction % 255 + 1;
    const report = buildControlRequest(transaction, opcode, options);
    this.transaction = transaction;
    const request = { transaction_id: transaction, opcode };
    const device = this.device;
    const generation = this.generation;
    this.outstanding = request;
    this.update({ busy: true, error: null });
    let sent = false;
    try {
      await withTimeout(device.sendFeatureReport(20, report), this.timeout);
      sent = true;
      if (generation !== this.generation) throw new Error("Command outcome uncertain after disconnect");
      const result = await this.pollResult(device, request);
      this.outstanding = null;
      if (result.status !== 2) throw new Error(`Command ${opcode}: status ${result.status}, error ${result.error_reason}`);
      if (opcode === 6) {
        const saved = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), this.timeout),
          this.state.hardware);
        if (device !== this.device || generation !== this.generation) {
          throw new Error("Disconnected during settings readback");
        }
        const selected = { system_clock_mhz: report[4] | report[5] << 8, sdcard_profile: report[6] };
        if (!sameSettings(saved, selected) || !sameSettings(result.settings, selected)) {
          throw new Error("Firmware did not confirm the selected values");
        }
        this.update({ settings: saved });
      }
      if (opcode === 3) this.update({ msc_command_pending: true });
      if (opcode === 4) this.update({ self_test_command_pending: true });
      if (opcode === 2) this.update({ reboot_pending: true });
      return result;
    } catch (error) {
      // SET rejection is authoritative: do not poll or accept a prior result.
      if (!sent && error.name !== "TimeoutError" && generation === this.generation) this.outstanding = null;
      if (generation === this.generation) {
        this.update({ outcome_uncertain: Boolean(this.outstanding), error: error.message });
      }
      throw error;
    } finally {
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

  async saveSettings(settings) {
    requireValid(validSettings(settings), "boot settings");
    if (!this.storageReady() || this.state.msc.enabled || this.state.self_test?.active) {
      this.update({ settings_message: "Fresh idle firmware-owned storage required before saving." });
      return false;
    }
    try {
      await this.command(6, { args: [settings.system_clock_mhz & 255,
        settings.system_clock_mhz >> 8, settings.sdcard_profile] });
      this.update({ settings_message: "Saved for next boot. Reboot separately to apply." });
      return true;
    } catch (error) {
      this.update({ settings_message: `Save failed: ${error.message}` });
      return false;
    }
  }
}
