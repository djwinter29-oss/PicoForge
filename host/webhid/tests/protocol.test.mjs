import assert from "node:assert/strict";
import test from "node:test";
import {
  boardStatusFromRuntime,
  decodeInfoReport,
  decodeFilesystemReport,
  decodeMemoryReport,
  decodeMscStatus,
  decodeStatusReport,
  INFO_REPORT_ID,
  matchesPicoForge,
  PicoForgeWebHid,
  STATUS_REPORT_ID,
  buildSelfTestCommand,
  decodeSelfTestReport,
} from "../js/device.mjs";

test("self-test start/cancel commands validate size and encode HID payloads", () => {
  assert.deepEqual([...buildSelfTestCommand("start", 32)], [2, 1, 32, 0, 0, 0]);
  assert.deepEqual([...buildSelfTestCommand("cancel")], [2, 0, 0, 0, 0, 0]);
  assert.deepEqual([...buildSelfTestCommand("start", 1024)], [2, 1, 0, 4, 0, 0]);
  for (const size of [0, 1025, 1.5, NaN]) assert.throws(() => buildSelfTestCommand("start", size));
});

test("one GiB self-test completion decodes exactly and rejects oversized telemetry", () => {
  const report = new Uint8Array(32);
  const view = new DataView(report.buffer);
  report[0] = 2;
  report[2] = 6;
  report[3] = 2;
  for (const offset of [4, 8, 12]) view.setUint32(offset, 1073741824, true);
  const status = decodeSelfTestReport(view);
  assert.equal(status.target_bytes, 1073741824);
  assert.equal(status.progress_percent, 100);
  view.setUint32(4, 1073741825, true);
  assert.throws(() => decodeSelfTestReport(view), /Invalid/);
});

test("self-test telemetry decodes progress, speeds, and cleanup flags", () => {
  const report = new Uint8Array(32);
  const view = new DataView(report.buffer);
  report[0] = 2;
  report[2] = 6;
  report[3] = 2;
  for (const offset of [4, 8, 12]) view.setUint32(offset, 33554432, true);
  view.setUint32(16, 16000, true);
  view.setUint32(20, 8000, true);
  const status = decodeSelfTestReport(view);
  assert.equal(status.state, "passed");
  assert.equal(status.progress_percent, 100);
  assert.equal(status.write_bytes_per_second, 2097152);
  assert.equal(status.read_bytes_per_second, 4194304);
  assert.equal(status.volume_mounted, true);
  report[3] = 3;
  assert.throws(() => decodeSelfTestReport(view), /Invalid/);
  report[3] = 2;
  report[30] = 11;
  assert.equal(decodeSelfTestReport(view).request_error, 11);
  report[30] = 12;
  assert.throws(() => decodeSelfTestReport(view), /Invalid/);
  report[30] = 0;
  report[31] = 1;
  assert.throws(() => decodeSelfTestReport(view), /Invalid/);
});

function infoReport({ reportId = true, mcu = 2, name = "PicoForge", serial = "0123456789abcdef" } = {}) {
  const payload = new Uint8Array(42);
  payload[0] = 2;
  payload[1] = mcu;
  payload.set(new TextEncoder().encode(name), 2);
  payload.set(new TextEncoder().encode(serial), 22);
  return reportId ? Uint8Array.from([1, ...payload]) : payload;
}

test("memory decoder rejects unsupported versions, components, and modes", () => {
  const bytes = new Uint8Array(29);
  const view = new DataView(bytes.buffer);
  bytes[0] = 3;
  assert.throws(() => decodeMemoryReport(3, view), /Unsupported HID memory report version/);
  bytes[0] = 4;
  assert.throws(() => decodeMemoryReport(3, view), /Unsupported HID memory report version/);
  bytes.set([2, 0, 0, 2, 2]);
  assert.throws(() => decodeMemoryReport(3, view), /Unknown HID memory type or mode/);
  bytes.set([2, 0, 0, 1, 3]);
  assert.throws(() => decodeMemoryReport(3, view), /Unknown HID memory type or mode/);
});

test("decode stable product, MCU, and serial identity", () => {
  assert.deepEqual(decodeInfoReport(new DataView(infoReport({ reportId: false }).buffer)), {
    version: 2,
    mcu_id: 2,
    mcu: "RP2350",
    product_name: "PicoForge",
    serial_number: "0123456789abcdef",
    firmware_version: null,
  });
});

test("identity layout three reports firmware release independently of protocol version", () => {
  const report = infoReport({ reportId: false });
  report[0] = 3;
  report.set([0, 1, 0], 38);
  for (const framed of [report, Uint8Array.from([1, ...report, 0]), Uint8Array.from([0, 1, ...report])]) {
    assert.equal(decodeInfoReport(new DataView(framed.buffer)).firmware_version, "0.1.0");
  }
  report[41] = 1;
  assert.throws(() => decodeInfoReport(new DataView(report.buffer)), /reserved/);
  report[41] = 0;
  report[0] = 4;
  assert.throws(() => decodeInfoReport(new DataView(report.buffer)), /version/);
});

test("accept zero-prefixed and padded standard feature reports", () => {
  const zeroPrefixed = Uint8Array.from([0, ...infoReport()]);
  assert.equal(decodeInfoReport(new DataView(zeroPrefixed.buffer)).product_name, "PicoForge");

  const padded = Uint8Array.from([...infoReport(), 0]);
  assert.equal(decodeInfoReport(new DataView(padded.buffer)).product_name, "PicoForge");
});

test("decode changing telemetry from periodic input report", () => {
  const payload = new Uint8Array(13);
  payload[0] = 2;
  payload[1] = 9;
  const view = new DataView(payload.buffer);
  view.setUint32(2, 123_456, true);
  view.setUint32(6, 150_000_000, true);
  payload[10] = 0x1f;
  view.setInt16(11, 2345, true);
  assert.deepEqual(decodeStatusReport(2, new DataView(payload.buffer)), {
    version: 2,
    sequence: 9,
    uptime_ms: 123_456,
    system_clock_hz: 150_000_000,
    usb_mounted: true,
    cdc_open: true,
    temperature_valid: true,
    temperature_centidegrees: 2345,
    led_available: true,
    led_on: true,
    cpu_usage_basis_points: null,
    ram_used_bytes: null,
    ram_total_bytes: null,
  });
});

test("runtime layout three decodes bounded CPU and RAM while rejecting malformed extensions", () => {
  const report = new Uint8Array(23);
  const view = new DataView(report.buffer);
  report[0] = 3;
  view.setUint32(6, 250_000_000, true);
  view.setUint16(13, 1234, true);
  view.setUint32(15, 65536, true);
  view.setUint32(19, 532480, true);
  const status = decodeStatusReport(2, view);
  assert.equal(status.cpu_usage_basis_points, 1234);
  assert.equal(status.ram_used_bytes, 65536);
  assert.equal(status.ram_total_bytes, 532480);
  assert.equal(decodeStatusReport(2, Uint8Array.from([...report, 0])).ram_used_bytes, 65536);
  assert.throws(() => decodeStatusReport(2, report.subarray(0, 22)), /size/);
  assert.throws(() => decodeStatusReport(2, Uint8Array.from([...report, 1])), /padding/);
  for (const cpu of [0, 10000, 0xffff]) {
    view.setUint16(13, cpu, true);
    assert.equal(decodeStatusReport(2, view).cpu_usage_basis_points, cpu === 0xffff ? null : cpu);
  }
  view.setUint16(13, 10001, true);
  assert.throws(() => decodeStatusReport(2, view), /CPU/);
  view.setUint16(13, 0, true);
  view.setUint32(19, 0, true);
  assert.throws(() => decodeStatusReport(2, view), /RAM/);
  view.setUint32(19, 65535, true);
  assert.throws(() => decodeStatusReport(2, view), /RAM/);
});

test("derive board status from the dynamic report", () => {
  assert.deepEqual(boardStatusFromRuntime({
    temperature_valid: true,
    temperature_centidegrees: 2345,
    led_available: true,
    led_on: true,
  }), { temperature_valid: true, temperature_centidegrees: 2345, led_available: true, led_on: true });
});

test("decode SD memory state, link settings, capacity, and CID", () => {
  const payload = new Uint8Array(29);
  const view = new DataView(payload.buffer);
  payload[0] = 2;
  payload[1] = 7;
  payload[2] = 0x3f;
  payload[3] = 1;
  payload[4] = 2;
  view.setUint32(5, 25_000_000, true);
  view.setUint32(9, 4096, true);
  payload.set([0x1b, 0x53, 0x44, 0x43, 0x41, 0x52, 0x44, 0x31,
    0x21, 0x12, 0x34, 0x56, 0x78, 0x01, 0xaa, 0xff], 13);
  assert.deepEqual(decodeMemoryReport(3, view), {
    version: 2,
    sequence: 7,
    configured: true,
    presence_known: true,
    present: true,
    initialized: true,
    component_type: "sd-card",
    mode: "spi",
    bus_clock_hz: 25_000_000,
    block_count: 4096,
    capacity_bytes: 2_097_152,
    manufacturer: {
      id: 0x1b,
      oem_id: "SD",
      product_name: "CARD1",
      product_revision: "2.1",
      serial_number: 0x12345678,
      manufacture_year: 2026,
      manufacture_month: 10,
    },
  });
});

test("decode mounted filesystem format reports", () => {
  assert.deepEqual(decodeFilesystemReport(7, Uint8Array.from([2, 8, 4])), {
    version: 2, sequence: 8, format: "exfat",
  });
  assert.throws(() => decodeFilesystemReport(7, Uint8Array.from([2, 8, 5])), /Unknown/);
});

test("distinguish unknown presence before card initialization", () => {
  const payload = new Uint8Array(29);
  payload[0] = 2;
  payload[2] = 0x01;
  payload[3] = 1;
  payload[4] = 1;
  const memory = decodeMemoryReport(3, new DataView(payload.buffer));
  assert.equal(memory.present, null);
  assert.equal(memory.capacity_bytes, null);
  assert.equal(memory.manufacturer, null);
});

test("decode MSC enable transition status", () => {
  assert.deepEqual(decodeMscStatus(Uint8Array.from([5, 2, 1, 0, 0])), {
    enabled: true, transition_pending: false, request_rejected: false, force_return_supported: false,
  });
  assert.deepEqual(decodeMscStatus(Uint8Array.from([2, 0, 1, 0])), {
    enabled: false, transition_pending: true, request_rejected: false, force_return_supported: false,
  });
  assert.equal(decodeMscStatus(Uint8Array.from([5, 3, 1, 0, 0])).force_return_supported, true);
  assert.throws(() => decodeMscStatus(Uint8Array.from([2, 2, 0, 0])), /Invalid/);
});

test("force return requires capability and sends a distinct command without weakening safe disable", async () => {
  const client = new PicoForgeWebHid(undefined);
  const sent = [];
  client.device = { sendFeatureReport: async (id, payload) => sent.push([id, ...payload]) };
  client.state.msc = { enabled: true, transition_pending: false };
  await assert.rejects(client.setMsc(false, true), /force-return support/);
  assert.equal(sent.length, 0);
  client.state.msc.force_return_supported = true;
  await client.setMsc(false, true);
  assert.deepEqual(sent, [[5, 3, 2]]);
  assert.equal(client.state.msc.transition_pending, true);
  await assert.rejects(client.setMsc(true, true), /disabling/);
  client.state.msc.transition_pending = false;
  await client.setMsc(false);
  assert.deepEqual(sent[1], [5, 2, 0]);
});

test("reject an LED-on dynamic status when no LED is available", () => {
  const payload = new Uint8Array(13);
  payload[0] = 2;
  new DataView(payload.buffer).setUint32(6, 125_000_000, true);
  payload[10] = 0x10;
  assert.throws(() => decodeStatusReport(2, new DataView(payload.buffer)), /without an available LED/);
});

test("reject wrong versions, clocks, padding, and device identities", () => {
  const badVersion = infoReport({ reportId: false });
  badVersion[0] = 1;
  assert.throws(() => decodeInfoReport(new DataView(badVersion.buffer)), /version/);
  assert.throws(() => decodeStatusReport(1, new DataView(new ArrayBuffer(7))), /report ID/);
  assert.equal(matchesPicoForge({ vendorId: 0xcafe, productId: 0x4011,
    collections: [{ usagePage: 0xff00, usage: 1 }] }), true);
  assert.equal(matchesPicoForge({ vendorId: 0xcafe, productId: 0x4012,
    collections: [{ usagePage: 0xff00, usage: 1 }] }), true);
});

test("Windows padded inputs permit MSC enable and reconnect after USB re-enumeration", async () => {
  class MockHidDevice extends EventTarget {
    constructor() {
      super();
      this.vendorId = 0xcafe;
      this.productId = 0x4011;
      this.serialNumber = "0123456789abcdef";
      this.collections = [{ usagePage: 0xff00, usage: 1 }];
      this.opened = false;
    }

    async open() { this.opened = true; }
    async close() { this.opened = false; }
    async receiveFeatureReport(reportId) {
      assert.equal(reportId, INFO_REPORT_ID);
      return new DataView(infoReport().buffer);
    }
    async sendFeatureReport(reportId, payload) {
      this.lastFeature = { reportId, payload: [...payload] };
    }
  }

  class MockHid extends EventTarget {
    constructor() {
      super();
      this.allowed = [];
      this.promptDevice = null;
    }

    async getDevices() { return this.allowed; }
    async requestDevice() { return this.promptDevice ? [this.promptDevice] : []; }
    emit(type, device) {
      const event = new Event(type);
      Object.defineProperty(event, "device", { value: device });
      this.dispatchEvent(event);
    }
  }

  const hid = new MockHid();
  const firstDevice = new MockHidDevice();
  hid.promptDevice = firstDevice;
  const client = new PicoForgeWebHid(hid);

  await client.connect();
  assert.equal(client.state.connected, true, client.state.error);
  function emitPaddedInput(reportId, bytes) {
    const buffer = new Uint8Array(36).fill(0xa5);
    buffer.set(bytes, 4);
    const event = new Event("inputreport");
    Object.defineProperties(event, {
      device: { value: firstDevice }, reportId: { value: reportId },
      data: { value: new DataView(buffer.buffer, 4, 32) },
    });
    firstDevice.dispatchEvent(event);
  }
  const runtime = new Uint8Array(23);
  const runtimeView = new DataView(runtime.buffer);
  runtime[0] = 3;
  runtimeView.setUint32(6, 250000000, true);
  runtimeView.setUint32(19, 532480, true);
  emitPaddedInput(2, runtime);
  assert.equal(client.state.error, null);
  assert.equal(client.state.runtime.system_clock_hz, 250000000);
  emitPaddedInput(5, [3, 0, 0, 0]);
  assert.equal(client.state.error, null);
  assert.equal(client.state.msc.enabled, false);
  assert.equal(client.state.msc.force_return_supported, true);
  emitPaddedInput(7, [2, 0, 4]);
  assert.equal(client.state.error, null);
  const memory = new Uint8Array(29);
  memory[0] = 2;
  emitPaddedInput(3, memory);
  assert.equal(client.state.error, null);
  emitPaddedInput(5, [3, 2, 0, 0]);
  assert.match(client.state.error, /Invalid HID MSC/);
  emitPaddedInput(5, [3, 0, 0, 0]);
  runtime[10] = 0x08;
  emitPaddedInput(2, runtime);
  await client.setLed(true);
  assert.deepEqual(firstDevice.lastFeature, { reportId: 4, payload: [2, 1] });
  await client.setLed(false);
  assert.deepEqual(firstDevice.lastFeature, { reportId: 4, payload: [2, 0] });
  await client.setMsc(true);
  assert.deepEqual(firstDevice.lastFeature, { reportId: 5, payload: [2, 1] });
  assert.equal(client.state.msc.transition_pending, true);

  hid.emit("disconnect", firstDevice);
  assert.equal(client.state.reconnecting, true);

  const reenumeratedDevice = new MockHidDevice();
  hid.allowed = [reenumeratedDevice];
  hid.emit("connect", reenumeratedDevice);
  await new Promise(resolve => setImmediate(resolve));

  assert.equal(client.state.connected, true);
  assert.equal(client.state.reconnecting, false);
  assert.equal(client.state.msc.transition_pending, true);

  const status = new Uint8Array(13);
  status[0] = 2;
  new DataView(status.buffer).setUint32(6, 150_000_000, true);
  const inputReport = new Event("inputreport");
  Object.defineProperties(inputReport, {
    device: { value: reenumeratedDevice },
    reportId: { value: STATUS_REPORT_ID },
    data: { value: new DataView(status.buffer) },
  });
  reenumeratedDevice.dispatchEvent(inputReport);
  assert.equal(client.state.runtime.system_clock_hz, 150_000_000);

  const enabledReport = new Event("inputreport");
  Object.defineProperties(enabledReport, {
    device: { value: reenumeratedDevice },
    reportId: { value: 5 },
    data: { value: new DataView(Uint8Array.from([2, 1, 0, 0]).buffer) },
  });
  reenumeratedDevice.dispatchEvent(enabledReport);
  assert.equal(client.state.msc.enabled, true);

  await client.setMsc(false);
  hid.emit("disconnect", reenumeratedDevice);
  assert.equal(client.state.reconnecting, true);

  const firmwareOwnedDevice = new MockHidDevice();
  hid.allowed = [firmwareOwnedDevice];
  hid.emit("connect", firmwareOwnedDevice);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, true);

  const disabledReport = new Event("inputreport");
  Object.defineProperties(disabledReport, {
    device: { value: firmwareOwnedDevice },
    reportId: { value: 5 },
    data: { value: new DataView(Uint8Array.from([2, 0, 0, 0]).buffer) },
  });
  firmwareOwnedDevice.dispatchEvent(disabledReport);
  assert.equal(client.state.msc.enabled, false);

  await client.setSelfTest("start", 32);
  assert.deepEqual(firmwareOwnedDevice.lastFeature, { reportId: 6, payload: [2, 1, 32, 0, 0, 0] });
  assert.equal(client.state.self_test_command_pending, true);
  const benchmark = new Uint8Array(32);
  benchmark[0] = 2;
  benchmark[2] = 3;
  new DataView(benchmark.buffer).setUint32(4, 33554432, true);
  const benchmarkReport = new Event("inputreport");
  Object.defineProperties(benchmarkReport, {
    device: { value: firmwareOwnedDevice }, reportId: { value: 6 },
    data: { value: new DataView(benchmark.buffer) },
  });
  firmwareOwnedDevice.dispatchEvent(benchmarkReport);
  assert.equal(client.state.self_test.active, true);
  assert.equal(client.state.self_test_command_pending, false);
  await client.setSelfTest("cancel");
  assert.deepEqual(firmwareOwnedDevice.lastFeature, { reportId: 6, payload: [2, 0, 0, 0, 0, 0] });
});

test("every true disconnect clears stale telemetry including self-test state", async () => {
  const stale = {
    hardware: {}, runtime: {}, board_status: {}, memory: {}, filesystem_format: "fat32", msc: {},
    self_test: { active: true }, self_test_command_pending: true, updated_at: "now",
  };
  const cleared = Object.fromEntries(Object.keys(stale).map(key => [key, key === "self_test_command_pending" ? false : null]));
  const newClient = (hid = new EventTarget()) => {
    const client = new PicoForgeWebHid(hid);
    client.update(stale);
    return client;
  };
  const device = { opened: true, removeEventListener() {}, addEventListener() {}, async close() {} };
  const expectCleared = client => assert.deepEqual(
    Object.fromEntries(Object.keys(stale).map(key => [key, client.state[key]])), cleared);

  let client = newClient();
  client.device = device;
  client.onDisconnect({ device });
  expectCleared(client);

  client = newClient();
  client.device = device;
  client.reconnectSerial = "0123456789abcdef";
  await client.disconnect();
  expectCleared(client);

  const hid = new EventTarget();
  hid.getDevices = async () => { throw new Error("denied"); };
  client = newClient(hid);
  await client.connect();
  expectCleared(client);
});