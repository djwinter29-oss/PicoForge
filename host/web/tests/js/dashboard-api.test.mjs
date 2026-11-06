// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import {
  acknowledge, DashboardError, reconciledResult, responseJson, savedSettings, scannedNetworks,
} from "../../src/js/dashboard-api.mjs";

test("mutation boundary distinguishes refusals, malformed replies, and unknown delivery", async () => {
  for (const body of [null, [], true, "ok"]) {
    await assert.rejects(responseJson({ ok: true, json: async () => body }, "save", true),
      error => error instanceof DashboardError && error.uncertain === true);
  }
  const invalid = { json: async () => { throw new SyntaxError("invalid JSON"); } };
  await assert.rejects(responseJson({ ok: true, ...invalid }, "save", true), { uncertain: true });
  await assert.rejects(responseJson({ ok: true, ...invalid }, "status"), { uncertain: false });
  for (const [status, body, uncertain] of [
    [409, { error: "busy" }, false], [404, { error: "unknown board" }, false],
    [503, { error: "not supported" }, false], [503, { error: "read failed" }, true],
    [503, { error: "refused", uncertain: false }, false], [409, { error: "pending", uncertain: true }, true],
  ]) {
    await assert.rejects(responseJson({ ok: false, status, json: async () => body }, "fallback", true),
      error => error.status === status && error.uncertain === uncertain);
  }
  await assert.rejects(responseJson({ ok: false, status: 409, ...invalid }, "known refusal", true),
    { status: 409, uncertain: false, message: "known refusal" });
});

test("scan boundary validates each network without changing raw SSID bytes", () => {
  const network = { ssid: "\\xFF\\x00", ssid_hex: "ff00", rssi: -30, secured: false, security: "Open" };
  assert.deepEqual(scannedNetworks({ networks: [network] }), [network]);
  assert.deepEqual(scannedNetworks({ networks: [] }), []);
  for (const invalid of [null, [], "network", { ...network, ssid: null }, { ...network, ssid_hex: "f" },
    { ...network, ssid_hex: "gg" }, { ...network, ssid_hex: "ff".repeat(33) },
    { ...network, secured: "false" }, { ...network, rssi: NaN }, { ...network, rssi: -129 },
    { ...network, rssi: 128 }, { ...network, rssi: -30.5 }, { ...network, security: {} }]) {
    assert.throws(() => scannedNetworks({ networks: [network, invalid] }), { uncertain: true });
  }
  for (const networks of [undefined, null, {}, "networks"])
    assert.throws(() => scannedNetworks({ networks }), { uncertain: true });
});

test("mutation success contracts accept both hosts and reject contradictory results", () => {
  for (const opcode of [1, 2, 4]) {
    assert.deepEqual(acknowledge({}, opcode), { status: "completed", opcode });
    assert.deepEqual(acknowledge({ control_result: { status: "completed", opcode } }, opcode),
      { status: "completed", opcode });
    for (const result of [{ ok: false }, { control_result: { status: "pending", opcode } },
      { control_result: { status: "completed", opcode: 255 } }])
      assert.throws(() => acknowledge(result, opcode), { uncertain: true });
  }
  const settings = { system_clock_mhz: 200, sdcard_profile: 0, radios: 3 };
  assert.deepEqual(savedSettings({ settings }, settings), settings);
  for (const result of [{}, { settings: { ...settings, radios: "3" } },
    { settings: { ...settings, system_clock_mhz: 250 } }])
    assert.throws(() => savedSettings(result, settings), { uncertain: true });
});

test("reconciliation accepts only terminal results or an explicitly clean host", () => {
  for (const control_result of [undefined, null])
    assert.equal(reconciledResult({ uncertain: false, control_result }), null);
  for (const status of ["completed", "invalid", "unsupported", "busy", "failed"]) {
    const control_result = { opcode: 4, status, error: "storage_busy", detail: 1 };
    assert.deepEqual(reconciledResult({ uncertain: false, control_result }), control_result);
  }
  for (const control_result of [{}, [], true, { opcode: 4, status: "pending" }, { opcode: 4, status: "idle" },
    { opcode: "4", status: "failed" }, { opcode: 0, status: "failed" }, { opcode: 14, status: "completed" },
    { opcode: 4.5, status: "failed" }, { opcode: 4, status: "failed", error: {} },
    { opcode: 4, status: "failed", detail: 256 }]) {
    assert.throws(() => reconciledResult({ uncertain: false, control_result }), { uncertain: true });
  }
  for (const uncertain of [undefined, null, true, "false"])
    assert.throws(() => reconciledResult({ uncertain }), { uncertain: true });
});
