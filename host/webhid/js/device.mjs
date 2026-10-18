export const DEVICE_FILTER = Object.freeze({
  vendorId: 0xcafe,
  productId: 0x4011,
  usagePage: 0xff00,
  usage: 0x01,
});

export const INFO_REPORT_ID = 1;
export const STATUS_REPORT_ID = 2;
export const MEMORY_REPORT_ID = 3;
export const LED_COMMAND_REPORT_ID = 4;
export const MSC_COMMAND_REPORT_ID = 5;
export const SELF_TEST_REPORT_ID = 6;
export const FILESYSTEM_REPORT_ID = 7;
const INFO_PAYLOAD_SIZE = 42;
const STATUS_PAYLOAD_SIZE = 23;
const MEMORY_PAYLOAD_SIZE = 29;
const MSC_STATUS_SIZE = 4;
const FILESYSTEM_PAYLOAD_SIZE = 3;

function asView(report) {
  if (report instanceof DataView) return report;
  if (report instanceof Uint8Array) return new DataView(report.buffer, report.byteOffset, report.byteLength);
  throw new Error("Invalid HID report data");
}

function inputPayload(reportId, report) {
  const view = asView(report);
  const size = ({
    [STATUS_REPORT_ID]: view.byteLength && view.getUint8(0) === 2 ? 13 : STATUS_PAYLOAD_SIZE,
    [MEMORY_REPORT_ID]: MEMORY_PAYLOAD_SIZE,
    [MSC_COMMAND_REPORT_ID]: MSC_STATUS_SIZE,
    [SELF_TEST_REPORT_ID]: 32,
    [FILESYSTEM_REPORT_ID]: FILESYSTEM_PAYLOAD_SIZE,
  })[reportId];
  // Windows uses the collection's maximum Input length; bytes beyond this report are not protocol fields.
  if (size === undefined || view.byteLength < size || view.byteLength > 32) {
    throw new Error(`Unexpected HID input report ${reportId} size`);
  }
  return new DataView(view.buffer, view.byteOffset, size);
}

function featurePayload(report, reportId, payloadSize) {
  const view = asView(report);
  let offset;
  if (view.byteLength === payloadSize) offset = 0;
  else if (view.byteLength >= payloadSize + 2
    && view.getUint8(0) === 0 && view.getUint8(1) === reportId) offset = 2;
  else if (view.byteLength >= payloadSize + 1 && view.getUint8(0) === reportId) offset = 1;
  else throw new Error("Unexpected HID feature report ID or size");
  for (let index = offset + payloadSize; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID feature report padding");
  }
  return new DataView(view.buffer, view.byteOffset + offset, payloadSize);
}

function paddedText(view, offset, length) {
  const bytes = new Uint8Array(view.buffer, view.byteOffset + offset, length);
  const terminator = bytes.indexOf(0);
  return new TextDecoder("ascii").decode(terminator < 0 ? bytes : bytes.subarray(0, terminator));
}

export function decodeInfoReport(report) {
  const payload = featurePayload(report, INFO_REPORT_ID, INFO_PAYLOAD_SIZE);
  const version = payload.getUint8(0);
  if (version !== 2 && version !== 3) throw new Error(`Unsupported HID info report version ${version}`);
  const mcuId = payload.getUint8(1);
  const productName = paddedText(payload, 2, 20);
  const serialNumber = paddedText(payload, 22, 16);
  if (!productName || !/^[0-9a-f]{16}$/i.test(serialNumber)) {
    throw new Error("Invalid HID product or serial identity");
  }
  for (let index = version === 2 ? 38 : 41; index < INFO_PAYLOAD_SIZE; index++) {
    if (payload.getUint8(index) !== 0) throw new Error("Unexpected nonzero HID identity reserved bytes");
  }
  return {
    version,
    mcu_id: mcuId,
    mcu: ({ 1: "RP2040", 2: "RP2350" })[mcuId] ?? `Unknown MCU (${mcuId})`,
    product_name: productName,
    serial_number: serialNumber,
    firmware_version: version === 3 ? [38, 39, 40].map(offset => payload.getUint8(offset)).join(".") : null,
  };
}

export function decodeStatusReport(reportId, report) {
  if (reportId !== STATUS_REPORT_ID) throw new Error(`Unexpected HID status report ID ${reportId}`);
  const view = asView(report);
  if (view.byteLength < 13) throw new Error("Unexpected HID status report size");
  const version = view.getUint8(0);
  if (version !== 2 && version !== 3) throw new Error(`Unsupported HID status report version ${version}`);
  const size = version === 3 ? STATUS_PAYLOAD_SIZE : 13;
  if (view.byteLength < size) throw new Error("Unexpected HID status report size");
  for (let index = size; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID status report padding");
  }
  const flags = view.getUint8(10);
  let cpuUsage = null, ramUsed = null, ramTotal = null;
  if (version === 3) {
    cpuUsage = view.getUint16(13, true);
    ramUsed = view.getUint32(15, true);
    ramTotal = view.getUint32(19, true);
    if (cpuUsage > 10000 && cpuUsage !== 0xffff) throw new Error("Invalid CPU usage in HID status report");
    if (ramTotal === 0 || ramUsed > ramTotal) throw new Error("Invalid RAM usage in HID status report");
    if (cpuUsage === 0xffff) cpuUsage = null;
  }
  if ((flags & ~0x1f) !== 0) throw new Error("Unknown HID status flags");
  const ledAvailable = Boolean(flags & 0x08);
  const ledOn = Boolean(flags & 0x10);
  if (ledOn && !ledAvailable) throw new Error("LED state reported without an available LED");
  const temperatureValid = Boolean(flags & 0x04);
  const systemClockHz = view.getUint32(6, true);
  if (systemClockHz === 0) throw new Error("Invalid system clock in HID status report");
  return {
    version,
    sequence: view.getUint8(1),
    uptime_ms: view.getUint32(2, true),
    system_clock_hz: systemClockHz,
    usb_mounted: Boolean(flags & 0x01),
    cdc_open: Boolean(flags & 0x02),
    temperature_valid: temperatureValid,
    temperature_centidegrees: temperatureValid ? view.getInt16(11, true) : null,
    led_available: ledAvailable,
    led_on: ledOn,
    cpu_usage_basis_points: cpuUsage,
    ram_used_bytes: ramUsed,
    ram_total_bytes: ramTotal,
  };
}

export function decodeMemoryReport(reportId, report) {
  if (reportId !== MEMORY_REPORT_ID) throw new Error(`Unexpected HID memory report ID ${reportId}`);
  const view = asView(report);
  if (view.byteLength < MEMORY_PAYLOAD_SIZE) throw new Error("Unexpected HID memory report size");
  for (let index = MEMORY_PAYLOAD_SIZE; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID memory report padding");
  }
  const version = view.getUint8(0);
  const sequence = view.getUint8(1);
  const flags = view.getUint8(2);
  const componentType = view.getUint8(3);
  const mode = view.getUint8(4);
  const busClockHz = view.getUint32(5, true);
  const blockCount = view.getUint32(9, true);
  const nor = [3, 4].includes(version) && componentType === 2;
  if (version !== 2 && !nor) throw new Error(`Unsupported HID memory report version ${version}`);
  if ((flags & ~0x3f) !== 0) throw new Error("Unknown HID memory flags");
  if (nor ? ![2, 3].includes(mode) : componentType > 1 || mode > 2) {
    throw new Error("Unknown HID memory type or mode");
  }
  const configured = Boolean(flags & 0x01);
  const presenceKnown = Boolean(flags & 0x02);
  const present = Boolean(flags & 0x04);
  const initialized = Boolean(flags & 0x08);
  const cidValid = Boolean(flags & 0x10);
  const capacityValid = Boolean(flags & 0x20);
  if (present && !presenceKnown) throw new Error("Memory reported present without known presence");
  if (initialized && (!configured || !presenceKnown || !present)) {
    throw new Error("Initialized memory requires a configured, present card");
  }
  if (cidValid !== initialized || capacityValid !== initialized
    || initialized !== Boolean(busClockHz && blockCount)) {
    throw new Error("Invalid memory metadata flags or values");
  }
  const cid = new Uint8Array(view.buffer, view.byteOffset + 13, 16);
  if (!initialized && cid.some(byte => byte !== 0)) throw new Error("Unexpected CID without initialized card");
  let manufacturer = null;
  if (nor && initialized) {
    if (cid[0] !== 0xef || cid[1] !== 0x40 || cid[2] !== 0x18
      || cid.subarray(version === 4 ? 15 : 7).some(byte => byte !== 0)
      || view.getUint32(16, true) !== 16777216 || blockCount * 512 > 16777216) {
      throw new Error("Invalid NOR identification or capacity");
    }
    manufacturer = { id: cid[0], product_name: "W25Q128", jedec_id: "ef4018",
      serial_number: null };
  } else if (cidValid) {
    const oemId = new TextDecoder("ascii").decode(cid.subarray(1, 3));
    const productName = new TextDecoder("ascii").decode(cid.subarray(3, 8));
    if (!/^[\x20-\x7e]{2}$/.test(oemId) || !/^[\x20-\x7e]{5}$/.test(productName)) {
      throw new Error("Invalid text in SD CID");
    }
    const date = ((cid[13] & 0x0f) << 8) | cid[14];
    manufacturer = {
      id: cid[0],
      oem_id: oemId,
      product_name: productName,
      product_revision: `${cid[8] >> 4}.${cid[8] & 0x0f}`,
      serial_number: new DataView(cid.buffer, cid.byteOffset + 9, 4).getUint32(0, false),
      manufacture_year: 2000 + (date >> 4),
      manufacture_month: date & 0x0f,
    };
  }
  return {
    version,
    sequence,
    configured,
    presence_known: presenceKnown,
    present: presenceKnown ? present : null,
    initialized,
    component_type: nor ? "nor-flash" : componentType === 1 ? "sd-card" : null,
    mode: ({ 1: "sdio", 2: "spi", 3: "qspi" })[mode] ?? null,
    bus_clock_hz: initialized ? busClockHz : null,
    block_count: capacityValid ? blockCount : null,
    capacity_bytes: capacityValid ? blockCount * 512 : null,
    manufacturer,
    ...(nor ? { physical_capacity_bytes: initialized ? view.getUint32(16, true) : null,
      device_uid: initialized && version === 4
        ? Array.from(cid.subarray(7, 15), byte => byte.toString(16).padStart(2, "0")).join("") : null } : {}),
  };
}

export function decodeFilesystemReport(reportId, report) {
  if (reportId !== FILESYSTEM_REPORT_ID) throw new Error(`Unexpected HID filesystem report ID ${reportId}`);
  const view = asView(report);
  if (view.byteLength < FILESYSTEM_PAYLOAD_SIZE) throw new Error("Unexpected HID filesystem report size");
  for (let index = FILESYSTEM_PAYLOAD_SIZE; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID filesystem report padding");
  }
  const version = view.getUint8(0);
  const sequence = view.getUint8(1);
  const formatId = view.getUint8(2);
  const format = ["unknown", "fat12", "fat16", "fat32", "exfat"][formatId];
  if (version !== 2) throw new Error(`Unsupported HID filesystem report version ${version}`);
  if (format === undefined) throw new Error("Unknown HID filesystem format");
  return { version, sequence, format };
}

export function decodeMscStatus(report) {
  const payload = featurePayload(report, MSC_COMMAND_REPORT_ID, MSC_STATUS_SIZE);
  const version = payload.getUint8(0);
  const enabled = payload.getUint8(1);
  const pending = payload.getUint8(2);
  const rejected = payload.getUint8(3);
  if (![2, 3].includes(version) || enabled > 1 || pending > 1 || rejected > 1) {
    throw new Error("Invalid HID MSC status report");
  }
  return { enabled: Boolean(enabled), transition_pending: Boolean(pending), request_rejected: Boolean(rejected),
    force_return_supported: version === 3 };
}

export function boardStatusFromRuntime(runtime) {
  return {
    temperature_valid: runtime.temperature_valid,
    temperature_centidegrees: runtime.temperature_centidegrees,
    led_available: runtime.led_available,
    led_on: runtime.led_on,
  };
}

export function buildSelfTestCommand(action, sizeMiB = 32) {
  if (action !== "start" && action !== "cancel") throw new Error("Self-test action must be start or cancel");
  if (action === "cancel") sizeMiB = 0;
  else if (!Number.isInteger(sizeMiB) || sizeMiB < 1 || sizeMiB > 1024) {
    throw new Error("Self-test size must be an integer from 1 to 1024 MiB");
  }
  const command = new Uint8Array(6);
  command[0] = 2;
  command[1] = action === "start" ? 1 : 0;
  new DataView(command.buffer).setUint32(2, sizeMiB, true);
  return command;
}

export function decodeSelfTestReport(report) {
  const payload = featurePayload(report, SELF_TEST_REPORT_ID, 32);
  const state = payload.getUint8(2);
  const flags = payload.getUint8(3);
  const target = payload.getUint32(4, true);
  const written = payload.getUint32(8, true);
  const read = payload.getUint32(12, true);
  const writeMs = payload.getUint32(16, true);
  const readMs = payload.getUint32(20, true);
  const error = payload.getUint8(28);
  const filesystemResult = payload.getUint8(29);
  const requestError = payload.getUint8(30);
  const mismatches = payload.getUint32(24, true);
  if (payload.getUint8(0) !== 2 || state > 8 || flags & ~3 || payload.getUint8(31)
    || error > 11 || requestError > 11 || filesystemResult > 19
    || target > 1024 * 1024 * 1024 || written > target || read > written
    || (state === 6 && (!target || written !== target || read !== target || mismatches || error || flags & 1))) {
    throw new Error("Invalid HID self-test report");
  }
  return {
    version: 2, sequence: payload.getUint8(1),
    state: ["idle", "queued", "mounting", "writing", "reading", "cleanup", "passed", "failed", "cancelled"][state],
    active: state >= 1 && state <= 5, target_bytes: target, written_bytes: written, read_bytes: read,
    write_elapsed_ms: writeMs, read_elapsed_ms: readMs,
    write_bytes_per_second: writeMs ? written * 1000 / writeMs : null,
    read_bytes_per_second: readMs ? read * 1000 / readMs : null,
    progress_percent: target ? (written + read) * 100 / (2 * target) : 0,
    mismatch_count: mismatches, error, filesystem_result: filesystemResult,
    request_error: requestError, file_remains: Boolean(flags & 1), volume_mounted: Boolean(flags & 2),
  };
}

export function matchesPicoForge(device) {
  return device.vendorId === DEVICE_FILTER.vendorId && [0x4011, 0x4012].includes(device.productId)
    && device.collections.some(collection => collection.usagePage === DEVICE_FILTER.usagePage
      && collection.usage === DEVICE_FILTER.usage);
}

export class PicoForgeWebHid {
  constructor(hid, onChange = () => {}) {
    this.hid = hid;
    this.onChange = onChange;
    this.device = null;
    this.state = {
      connected: false,
      reconnecting: false,
      error: hid ? null : "WebHID is unavailable; use a supported browser on localhost or HTTPS",
      hardware: null,
      runtime: null,
      board_status: null,
      memory: null,
      filesystem_format: null,
      self_test: null,
      self_test_command_pending: false,
      updated_at: null,
      busy: false,
    };
    this.reconnectSerial = null;
    this.onInputReport = event => {
      if (event.device !== this.device) return;
      try {
        if (![STATUS_REPORT_ID, MEMORY_REPORT_ID, FILESYSTEM_REPORT_ID,
          MSC_COMMAND_REPORT_ID, SELF_TEST_REPORT_ID].includes(event.reportId)) return;
        const payload = inputPayload(event.reportId, event.data);
        if (event.reportId === STATUS_REPORT_ID) {
          const runtime = decodeStatusReport(event.reportId, payload);
          this.update({ runtime, board_status: boardStatusFromRuntime(runtime),
            updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === MEMORY_REPORT_ID) {
          const memory = decodeMemoryReport(event.reportId, payload);
          this.update({ memory, updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === FILESYSTEM_REPORT_ID) {
          const filesystem = decodeFilesystemReport(event.reportId, payload);
          this.update({ filesystem_format: filesystem.format, updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === MSC_COMMAND_REPORT_ID) {
          const msc = decodeMscStatus(payload);
          this.update({ msc, updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === SELF_TEST_REPORT_ID) {
          const selfTest = decodeSelfTestReport(payload);
          this.update({ self_test: selfTest, self_test_command_pending: false,
            updated_at: new Date().toISOString(), error: null });
        }
      } catch (error) {
        this.update({ error: error.message });
      }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      event.device.removeEventListener("inputreport", this.onInputReport);
      this.device = null;
      if (this.reconnectSerial) {
        this.update({ connected: false, reconnecting: true, busy: false,
          filesystem_format: null, error: null });
      } else {
        this.update({ connected: false, reconnecting: false, hardware: null, runtime: null,
          board_status: null, memory: null, filesystem_format: null, updated_at: null, msc: null, busy: false,
          error: "Device disconnected" });
      }
    };
    this.onConnect = event => {
      if (!this.reconnectSerial || event.device.serialNumber !== this.reconnectSerial ||
          !matchesPicoForge(event.device) || this.device) return;
      void this.attachDevice(event.device);
    };
    hid?.addEventListener("disconnect", this.onDisconnect);
    hid?.addEventListener("connect", this.onConnect);
  }

  update(changes) {
    this.state = { ...this.state, ...changes };
    this.onChange(this.state);
  }

  async connect() {
    if (!this.hid || this.state.busy || this.device || this.state.reconnecting) return;
    this.update({ busy: true, error: null });
    try {
      const allowed = (await this.hid.getDevices()).filter(matchesPicoForge);
      let selected = allowed.length === 1 ? allowed[0] : null;
      if (!selected) {
        const devices = await this.hid.requestDevice({ filters: [0x4011, 0x4012].map(productId => ({
          ...DEVICE_FILTER, productId,
        })) });
        if (devices.length === 0) {
          this.update({ busy: false });
          return;
        }
        if (devices.length !== 1 || !matchesPicoForge(devices[0])) {
          throw new Error("Select one PicoForge vendor HID interface");
        }
        [selected] = devices;
      }
      await this.attachDevice(selected);
    } catch (error) {
      this.update({ connected: false, busy: false, reconnecting: false, error: error.message,
        hardware: null, runtime: null, board_status: null, memory: null, msc: null, updated_at: null });
    }
  }

  async attachDevice(selected) {
    this.update({ busy: true, error: null });
    try {
      await selected.open();
      selected.addEventListener("inputreport", this.onInputReport);
      this.device = selected;
      const hardware = decodeInfoReport(await selected.receiveFeatureReport(INFO_REPORT_ID));
      this.reconnectSerial = selected.serialNumber || hardware.serial_number;
      this.update({ connected: true, busy: false, reconnecting: false, error: null, hardware,
        runtime: null, board_status: null, memory: null, self_test: null,
        self_test_command_pending: false, updated_at: null });
    } catch (error) {
      selected.removeEventListener("inputreport", this.onInputReport);
      if (selected.opened) {
        try { await selected.close(); } catch { }
      }
      if (this.device === selected) this.device = null;
      this.update({ connected: false, busy: false, reconnecting: false, error: error.message });
    }
  }

  async setLed(on) {
    if (typeof on !== "boolean") throw new Error("LED state must be boolean");
    if (!this.device || this.state.busy || !this.state.board_status?.led_available) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(LED_COMMAND_REPORT_ID, Uint8Array.from([2, on ? 1 : 0]));
      const boardStatus = { ...this.state.board_status, led_on: on };
      const runtime = this.state.runtime ? { ...this.state.runtime, led_on: on } : null;
      this.update({ runtime, board_status: boardStatus, busy: false });
    } catch (error) {
      this.update({ busy: false, error: error.message });
    }
  }

  async setMsc(enable, force = false) {
    if (typeof enable !== "boolean") throw new Error("MSC state must be boolean");
    if (typeof force !== "boolean" || (force && enable)) throw new Error("Force return requires disabling MSC");
    if (force && (!this.state.msc?.enabled || !this.state.msc?.force_return_supported
      || this.state.msc?.transition_pending)) throw new Error("Enabled MSC with force-return support is unavailable");
    if (!this.device || this.state.busy) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(MSC_COMMAND_REPORT_ID,
        Uint8Array.from(force ? [3, 2] : [2, enable ? 1 : 0]));
      const msc = { ...(this.state.msc ?? {}), transition_pending: true, request_rejected: false };
      this.update({ msc, busy: false });
    } catch (error) {
      this.update({ busy: false, error: error.message });
    }
  }

  async disconnect() {
    const device = this.device;
    if (!device || this.state.busy) return;
    const reconnectSerial = this.reconnectSerial;
    this.update({ busy: true });
    device.removeEventListener("inputreport", this.onInputReport);
    try {
      if (device.opened) await device.close();
      this.device = null;
      this.reconnectSerial = null;
      this.update({ connected: false, busy: false, reconnecting: false, error: null,
        hardware: null, runtime: null, board_status: null, memory: null, msc: null, updated_at: null });
    } catch (error) {
      this.reconnectSerial = reconnectSerial;
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ busy: false, error: error.message });
    }
  }

  async setSelfTest(action, sizeMiB = 32) {
    const command = buildSelfTestCommand(action, sizeMiB);
    if (!this.device || this.state.busy || this.state.self_test_command_pending) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(SELF_TEST_REPORT_ID, command);
      this.update({ busy: false, self_test_command_pending: true });
    } catch (error) {
      this.update({ busy: false, self_test_command_pending: false, error: error.message });
    }
  }
}