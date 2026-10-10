export const DEVICE_FILTER = Object.freeze({
  vendorId: 0xcafe,
  productId: 0x4011,
  usagePage: 0xff00,
  usage: 0x01,
});

export const INFO_REPORT_ID = 1;
export const STATUS_REPORT_ID = 2;
export const BOARD_STATUS_REPORT_ID = 3;
export const LED_COMMAND_REPORT_ID = 4;

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
  const payload = featurePayload(report, INFO_REPORT_ID, 26);
  const version = payload.getUint8(0);
  if (version !== 1) throw new Error(`Unsupported HID info report version ${version}`);
  const mcuId = payload.getUint8(1);
  const systemClockHz = payload.getUint32(2, true);
  const productName = paddedText(payload, 6, 20);
  if (systemClockHz === 0 || !productName) throw new Error("Invalid HID hardware information");
  return {
    version,
    mcu_id: mcuId,
    mcu: ({ 1: "RP2040", 2: "RP2350" })[mcuId] ?? `Unknown MCU (${mcuId})`,
    system_clock_hz: systemClockHz,
    product_name: productName,
  };
}

export function decodeStatusReport(reportId, report) {
  if (reportId !== STATUS_REPORT_ID) throw new Error(`Unexpected HID status report ID ${reportId}`);
  let view = asView(report);
  if (view.byteLength === 8 && view.getUint8(0) === STATUS_REPORT_ID) {
    view = new DataView(view.buffer, view.byteOffset + 1, 7);
  }
  if (view.byteLength < 7) throw new Error("Unexpected HID status report size");
  for (let index = 7; index < view.byteLength; index++) {
    if (view.getUint8(index) !== 0) throw new Error("Unexpected HID status report padding");
  }
  const version = view.getUint8(0);
  const flags = view.getUint8(6);
  if (version !== 1) throw new Error(`Unsupported HID status report version ${version}`);
  if ((flags & ~0x03) !== 0) throw new Error("Unknown HID status flags");
  return {
    version,
    sequence: view.getUint8(1),
    uptime_ms: view.getUint32(2, true),
    usb_mounted: Boolean(flags & 0x01),
    cdc_open: Boolean(flags & 0x02),
  };
}

export function decodeBoardStatusReport(report) {
  const payload = featurePayload(report, BOARD_STATUS_REPORT_ID, 4);
  const version = payload.getUint8(0);
  const flags = payload.getUint8(1);
  if (version !== 1) throw new Error(`Unsupported HID board-status report version ${version}`);
  if ((flags & ~0x07) !== 0) throw new Error("Unknown HID board-status flags");
  const ledAvailable = Boolean(flags & 0x02);
  const ledOn = Boolean(flags & 0x04);
  if (ledOn && !ledAvailable) throw new Error("LED state reported without an available LED");
  return {
    temperature_valid: Boolean(flags & 0x01),
    temperature_centidegrees: flags & 0x01 ? payload.getInt16(2, true) : null,
    led_available: ledAvailable,
    led_on: ledOn,
  };
}

export function matchesPicoForge(device) {
  return device.vendorId === DEVICE_FILTER.vendorId && device.productId === DEVICE_FILTER.productId
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
      updated_at: null,
      busy: false,
    };
    this.onInputReport = event => {
      if (event.device !== this.device || event.reportId !== STATUS_REPORT_ID) return;
      try {
        this.update({ runtime: decodeStatusReport(event.reportId, event.data),
          updated_at: new Date().toISOString(), error: null });
      } catch (error) {
        this.update({ error: error.message });
      }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      event.device.removeEventListener("inputreport", this.onInputReport);
      this.device = null;
      this.update({ connected: false, hardware: null, runtime: null, board_status: null, updated_at: null,
        busy: false, error: "Device disconnected" });
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
      const devices = await this.hid.requestDevice({ filters: [{ ...DEVICE_FILTER }] });
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
      const boardStatus = decodeBoardStatusReport(await selected.receiveFeatureReport(BOARD_STATUS_REPORT_ID));
      hardware.serial_number = selected.serialNumber || "";
      this.update({ connected: true, busy: false, error: null, hardware,
        runtime: null, board_status: boardStatus, updated_at: null });
    } catch (error) {
      selected?.removeEventListener("inputreport", this.onInputReport);
      if (selected?.opened) {
        try { await selected.close(); } catch { }
      }
      this.device = null;
      this.update({ connected: false, busy: false, error: error.message,
        hardware: null, runtime: null, board_status: null, updated_at: null });
    }
  }

  async refresh() {
    if (!this.device || this.state.busy) return;
    this.update({ busy: true, error: null });
    try {
      const hardware = decodeInfoReport(await this.device.receiveFeatureReport(INFO_REPORT_ID));
      const boardStatus = decodeBoardStatusReport(await this.device.receiveFeatureReport(BOARD_STATUS_REPORT_ID));
      hardware.serial_number = this.device.serialNumber || "";
      this.update({ hardware, board_status: boardStatus, busy: false });
    } catch (error) {
      this.update({ busy: false, error: error.message });
    }
  }

  async setLed(on) {
    if (typeof on !== "boolean") throw new Error("LED state must be boolean");
    if (!this.device || this.state.busy || !this.state.board_status?.led_available) return;
    this.update({ busy: true, error: null });
    try {
      await this.device.sendFeatureReport(LED_COMMAND_REPORT_ID, Uint8Array.from([1, on ? 1 : 0]));
      const boardStatus = decodeBoardStatusReport(await this.device.receiveFeatureReport(BOARD_STATUS_REPORT_ID));
      this.update({ board_status: boardStatus, busy: false });
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
        hardware: null, runtime: null, board_status: null, updated_at: null });
    } catch (error) {
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ busy: false, error: error.message });
    }
  }
}