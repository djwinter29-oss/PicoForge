import assert from "node:assert/strict";
import test from "node:test";
import {
  boardStatusFromRuntime,
  decodeInfoReport,
  decodeMemoryReport,
  decodeMscStatus,
  decodeStatusReport,
  matchesPicoForge,
} from "../js/device.mjs";

function infoReport({ reportId = true, mcu = 2, name = "PicoForge", serial = "0123456789abcdef" } = {}) {
  const payload = new Uint8Array(42);
  payload[0] = 2;
  payload[1] = mcu;
  payload.set(new TextEncoder().encode(name), 2);
  payload.set(new TextEncoder().encode(serial), 22);
  return reportId ? Uint8Array.from([1, ...payload]) : payload;
}

test("decode stable product, MCU, and serial identity", () => {
  assert.deepEqual(decodeInfoReport(new DataView(infoReport({ reportId: false }).buffer)), {
    version: 2,
    mcu_id: 2,
    mcu: "RP2350",
    product_name: "PicoForge",
    serial_number: "0123456789abcdef",
  });
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
  });
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
    enabled: true, transition_pending: false, request_rejected: false,
  });
  assert.deepEqual(decodeMscStatus(Uint8Array.from([2, 0, 1, 0])), {
    enabled: false, transition_pending: true, request_rejected: false,
  });
  assert.throws(() => decodeMscStatus(Uint8Array.from([2, 2, 0, 0])), /Invalid/);
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