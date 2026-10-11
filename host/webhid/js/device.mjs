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
const INFO_PAYLOAD_SIZE = 42;
const STATUS_PAYLOAD_SIZE = 13;
const MEMORY_PAYLOAD_SIZE = 29;
const MSC_STATUS_SIZE = 4;

function asView(report) {
  if (report instanceof DataView) return report;
  if (report instanceof Uint8Array) return new DataView(report.buffer, report.byteOffset, report.byteLength);
  throw new Error("Invalid HID report data");
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
  if (version !== 2) throw new Error(`Unsupported HID info report version ${version}`);
  const mcuId = payload.getUint8(1);
  const productName = paddedText(payload, 2, 20);
  const serialNumber = paddedText(payload, 22, 16);
  if (!productName || !/^[0-9a-f]{16}$/i.test(serialNumber)) {
    throw new Error("Invalid HID product or serial identity");
  }
  for (let index = 38; index < INFO_PAYLOAD_SIZE; index++) {
    if (payload.getUint8(index) !== 0) throw new Error("Unexpected nonzero HID identity reserved bytes");
  }
  return {
    version,
    mcu_id: mcuId,
    mcu: ({ 1: "RP2040", 2: "RP2350" })[mcuId] ?? `Unknown MCU (${mcuId})`,
    product_name: productName,
    serial_number: serialNumber,
  };
}

export function decodeStatusReport(reportId, report) {
  if (reportId !== STATUS_REPORT_ID) throw new Error(`Unexpected HID status report ID ${reportId}`);
  const view = asView(report);
  if (view.byteLength < STATUS_PAYLOAD_SIZE) throw new Error("Unexpected HID status report size");
  for (let index = STATUS_PAYLOAD_SIZE; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID status report padding");
  }
  const version = view.getUint8(0);
  const flags = view.getUint8(10);
  if (version !== 2) throw new Error(`Unsupported HID status report version ${version}`);
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
  if (version !== 2) throw new Error(`Unsupported HID memory report version ${version}`);
  if ((flags & ~0x3f) !== 0) throw new Error("Unknown HID memory flags");
  if (componentType > 1 || mode > 2) throw new Error("Unknown HID memory type or mode");
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
  if (cidValid) {
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
    component_type: componentType === 1 ? "sd-card" : null,
    mode: ({ 1: "sdio", 2: "spi" })[mode] ?? null,
    bus_clock_hz: initialized ? busClockHz : null,
    block_count: capacityValid ? blockCount : null,
    capacity_bytes: capacityValid ? blockCount * 512 : null,
    manufacturer,
  };
}

export function decodeMscStatus(report) {
  const payload = featurePayload(report, MSC_COMMAND_REPORT_ID, MSC_STATUS_SIZE);
  const version = payload.getUint8(0);
  const enabled = payload.getUint8(1);
  const pending = payload.getUint8(2);
  const rejected = payload.getUint8(3);
  if (version !== 2 || enabled > 1 || pending > 1 || rejected > 1) {
    throw new Error("Invalid HID MSC status report");
  }
  return { enabled: Boolean(enabled), transition_pending: Boolean(pending), request_rejected: Boolean(rejected) };
}

export function boardStatusFromRuntime(runtime) {
  return {
    temperature_valid: runtime.temperature_valid,
    temperature_centidegrees: runtime.temperature_centidegrees,
    led_available: runtime.led_available,
    led_on: runtime.led_on,
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
      error: hid ? null : "WebHID is unavailable; use a supported browser on localhost or HTTPS",
      hardware: null,
      runtime: null,
      board_status: null,
      memory: null,
      updated_at: null,
      busy: false,
    };
    this.onInputReport = event => {
      if (event.device !== this.device) return;
      try {
        if (event.reportId === STATUS_REPORT_ID) {
          const runtime = decodeStatusReport(event.reportId, event.data);
          this.update({ runtime, board_status: boardStatusFromRuntime(runtime),
            updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === MEMORY_REPORT_ID) {
          const memory = decodeMemoryReport(event.reportId, event.data);
          this.update({ memory, updated_at: new Date().toISOString(), error: null });
        } else if (event.reportId === MSC_COMMAND_REPORT_ID) {
          const msc = decodeMscStatus(event.data);
          this.update({ msc, updated_at: new Date().toISOString(), error: null });
        }
      } catch (error) {
        this.update({ error: error.message });
      }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      event.device.removeEventListener("inputreport", this.onInputReport);
      this.device = null;
      this.update({ connected: false, hardware: null, runtime: null, board_status: null, memory: null, updated_at: null,
        msc: null, busy: false, error: "Device disconnected" });
    };
    hid?.addEventListener("disconnect", this.onDisconnect);
  }

  update(changes) {
    this.state = { ...this.state, ...changes };
    this.onChange(this.state);
  }

  async connect() {
    if (!this.hid || this.state.busy || this.device) return;
    this.update({ busy: true, error: null });
    let selected;
    try {
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
      selected = devices[0];
      await selected.open();
      selected.addEventListener("inputreport", this.onInputReport);
      this.device = selected;
      const hardware = decodeInfoReport(await selected.receiveFeatureReport(INFO_REPORT_ID));
      this.update({ connected: true, busy: false, error: null, hardware,
        runtime: null, board_status: null, memory: null, msc: null, updated_at: null });
    } catch (error) {
      selected?.removeEventListener("inputreport", this.onInputReport);
      if (selected?.opened) {
        try { await selected.close(); } catch { }
      }
      this.device = null;
      this.update({ connected: false, busy: false, error: error.message,
        hardware: null, runtime: null, board_status: null, memory: null, msc: null, updated_at: null });
    }
  }

  async setLed(on) {
    if (typeof on !== "boolean") throw new Error("LED state must be boolean");
    if (!this.device || this.state.busy || !this.state.board_status?.led_available) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(LED_COMMAND_REPORT_ID, Uint8Array.from([1, on ? 1 : 0]));
      const boardStatus = { ...this.state.board_status, led_on: on };
      const runtime = this.state.runtime ? { ...this.state.runtime, led_on: on } : null;
      this.update({ runtime, board_status: boardStatus, busy: false });
    } catch (error) {
      this.update({ busy: false, error: error.message });
    }
  }

  async setMsc(enable) {
    if (typeof enable !== "boolean") throw new Error("MSC state must be boolean");
    if (!this.device || this.state.busy) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(MSC_COMMAND_REPORT_ID, Uint8Array.from([2, enable ? 1 : 0]));
      const msc = { ...(this.state.msc ?? {}), transition_pending: true, request_rejected: false };
      this.update({ msc, busy: false });
    } catch (error) {
      this.update({ busy: false, error: error.message });
    }
  }

  async disconnect() {
    const device = this.device;
    if (!device || this.state.busy) return;
    this.update({ busy: true });
    device.removeEventListener("inputreport", this.onInputReport);
    try {
      if (device.opened) await device.close();
      this.device = null;
      this.update({ connected: false, busy: false, error: null,
        hardware: null, runtime: null, board_status: null, memory: null, msc: null, updated_at: null });
    } catch (error) {
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ busy: false, error: error.message });
    }
  }
}