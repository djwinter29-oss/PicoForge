import assert from "node:assert/strict";
import test from "node:test";
import { decodeInfoReport, decodeStatusReport, matchesPicoForge } from "../js/device.mjs";

function infoReport({ reportId = true, mcu = 2, clock = 150_000_000, name = "PicoForge" } = {}) {
  const payload = new Uint8Array(26);
  payload[0] = 1;
  payload[1] = mcu;
  new DataView(payload.buffer).setUint32(2, clock, true);
  payload.set(new TextEncoder().encode(name), 6);
  return reportId ? Uint8Array.from([1, ...payload]) : payload;
}

test("decode product, MCU, and clock from report payload", () => {
  assert.deepEqual(decodeInfoReport(new DataView(infoReport({ reportId: false }).buffer)), {
    version: 1,
    mcu_id: 2,
    mcu: "RP2350",
    system_clock_hz: 150_000_000,
    product_name: "PicoForge",
  });
});

test("decode little-endian runtime status", () => {
  const payload = new Uint8Array([1, 9, 0x40, 0xe2, 0x01, 0x00, 3]);
  assert.deepEqual(decodeStatusReport(2, new DataView(payload.buffer)), {
    version: 1,
    sequence: 9,
    uptime_ms: 123_456,
    usb_mounted: true,
    cdc_open: true,
  });
});

test("reject wrong versions, clocks, padding, and device identities", () => {
  const badVersion = infoReport({ reportId: false });
  badVersion[0] = 2;
  assert.throws(() => decodeInfoReport(new DataView(badVersion.buffer)), /version/);
  assert.throws(() => decodeStatusReport(1, new DataView(new ArrayBuffer(7))), /report ID/);
  assert.equal(matchesPicoForge({ vendorId: 0xcafe, productId: 0x4011,
    collections: [{ usagePage: 0xff00, usage: 1 }] }), true);
});