import assert from "node:assert/strict";
import test from "node:test";
import {
  boardStatusFromRuntime,
  decodeInfoReport,
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
});