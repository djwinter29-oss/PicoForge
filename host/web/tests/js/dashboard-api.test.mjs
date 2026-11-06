import assert from "node:assert/strict";
import test from "node:test";
import { acknowledge, DashboardError, responseJson, savedSettings } from "../../src/js/dashboard-api.mjs";

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

test("mutation success contracts accept both hosts and reject contradictory results", () => {
  for (const opcode of [1, 2, 4]) {
    acknowledge({}, opcode);
    acknowledge({ control_result: { status: "completed", opcode } }, opcode);
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
