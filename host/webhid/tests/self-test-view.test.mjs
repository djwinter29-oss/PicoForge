import assert from "node:assert/strict";
import test from "node:test";
import { renderStatus, renderSelfTest } from "../js/view.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../js/device.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

function withDocument(run) {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", value: "0",
      valueAsNumber: 32, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({}) };
  try { run(element); } finally { globalThis.document = saved; }
}

function snapshot() {
  const hardware = decodeIdentityReport(identity());
  return { connected: true, hardware, settings: decodeConfigReport(config(), hardware),
    freshness: { 3: "fresh", 10: "fresh", 11: "fresh" }, self_test: decodeSelfTestReport(selfTest()),
    memory: { sample_valid: true, initialized: true, configured: true, presence_known: true, present: true },
    msc: { enabled: false, transition_pending: false }, filesystem_format: "exfat" };
}

test("self-test writes require fresh, known, mounted, firmware-owned state and valid size", () => withDocument(el => {
  const state = snapshot();
  renderSelfTest(state);
  assert.equal(el("#self-test-start").disabled, false);
  for (const mutate of [
    value => { value.freshness[10] = "stale"; },
    value => { value.freshness[11] = "unavailable"; },
    value => { value.hardware.capabilities &= ~32; },
    value => { value.self_test.valid_for_control = false; },
    value => { value.self_test.volume_mounted = false; },
    value => { value.msc.enabled = true; },
    value => { value.msc.transition_pending = true; },
    value => { value.msc_command_pending = true; },
    value => { value.self_test_command_pending = true; },
    value => { value.outcome_uncertain = true; },
    value => { value.busy = true; },
  ]) {
    const changed = snapshot(); mutate(changed);
    renderSelfTest(changed);
    assert.equal(el("#self-test-start").disabled, true);
  }
  for (const size of [0, 1025, 1.5, NaN]) {
    el("#self-test-size").valueAsNumber = size;
    renderSelfTest(state); assert.equal(el("#self-test-start").disabled, true);
  }
  el("#self-test-size").valueAsNumber = 1024;
  renderSelfTest(state); assert.equal(el("#self-test-start").disabled, false);
  state.self_test = { ...state.self_test, state: "writing", active: true };
  renderSelfTest(state);
  assert.equal(el("#self-test-cancel").disabled, false);
  assert.equal(el("#self-test-start").disabled, true);
  state.self_test.state = "cleanup"; renderSelfTest(state);
  assert.equal(el("#self-test-cancel").disabled, true);
}));

test("self-test presents retained throughput, request errors, and unknown enums", () => withDocument(el => {
  const state = snapshot();
  state.self_test = decodeSelfTestReport(selfTest(6));
  state.self_test.request_error = 11;
  renderSelfTest(state);
  assert.equal(el("#self-test-state").textContent, "Passed");
  assert.equal(el("#self-test-write-speed").textContent, "2.00 MiB/s");
  assert.equal(el("#self-test-read-speed").textContent, "4.00 MiB/s");
  assert.equal(el("#self-test-progress").value, 100);
  assert.match(el("#self-test-message").textContent, /Filesystem not mounted/);
  state.self_test = decodeSelfTestReport(selfTest(99));
  state.self_test.request_error = 255;
  renderSelfTest(state);
  assert.equal(el("#self-test-state").textContent, "Unknown state (99)");
  assert.match(el("#self-test-message").textContent, /Unknown error \(255\)/);
  assert.equal(el("#self-test-start").disabled, true);
}));

test("identity RAM stays build accounting while stale inputs show no current values", () => withDocument(el => {
  const state = snapshot();
  state.runtime = { uptime_ms: 9007199254740993n, system_clock_hz: 200000000,
    cpu_usage_core0_basis_points: 1234, cpu_usage_core1_basis_points: null };
  state.board_status = { led_available: true, led_on: true };
  renderStatus(state);
  assert.equal(el("#ram-reserved").textContent, "64.0 KiB (12.3%)");
  assert.equal(el("#ram-total").textContent, "520 KiB");
  assert.equal(el("#cpu-usage-core0").textContent, "12.3%");
  assert.equal(el("#cpu-usage-core1").textContent, "--");
  assert.match(el("#uptime").textContent, /104249991d/);
  assert.equal(el("#led-control")["aria-pressed"], "true");
  assert.match(el("#board-config").textContent, /CLK\/CMD\/DAT0–3/);
  assert.deepEqual(el("#settings-clock").children.map(option => option.value), ["0", "150", "200", "250"]);
  state.runtime = null; state.board_status = null; state.memory = null; state.msc = null; state.self_test = null;
  state.freshness = { 3: "stale", 10: "stale", 11: "stale" };
  renderStatus(state);
  assert.equal(el("#identity-clock").textContent, "--");
  assert.equal(el("#cpu-usage-core0").textContent, "--");
  assert.equal(el("#ram-total").textContent, "520 KiB");
  assert.equal(el("#telemetry-freshness").textContent, "telemetry: stale");
  assert.equal(el("#storage-freshness").textContent, "storage: stale");
  assert.equal(el("#led-control")["aria-pressed"], "false");
}));

test("storage does not infer absence from an unavailable sample or mount from cached format", () => withDocument(el => {
  const state = snapshot();
  state.memory.sample_valid = false; state.memory.presence_known = false; state.memory.initialized = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="memory-title"]').hidden, false);
  assert.match(el("#memory-note").textContent, /does not mean the card is absent/);
  state.memory.sample_valid = true; state.memory.presence_known = true; state.memory.present = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="memory-title"]').hidden, true);
  state.hardware.capabilities &= ~32;
  state.memory.presence_known = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
}));
