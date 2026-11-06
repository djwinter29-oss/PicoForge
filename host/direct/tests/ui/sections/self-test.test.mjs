import assert from "node:assert/strict";
import test from "node:test";
import { renderSelfTest, deriveSelfTestViewModel } from "../../../web/js/ui/sections/self-test.mjs";
import { decodeSelfTestReport } from "../../../web/js/protocol/codec.mjs";
import { selfTest } from "../../support/fixtures.mjs";
import { withDocument, snapshot } from "../../support/view.mjs";

test("self-test writes require fresh, known, mounted, firmware-owned state and valid size", () => withDocument((el, renderStatus) => {
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
    value => { value.pending_msc = true; },
    value => { value.pending_self_test = { sequence: 0 }; },
    value => { value.outcome_uncertain = true; },
    value => { value.busy = true; },
    value => { value.link = "ble"; },
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
  assert.equal(el("#self-test-disclosure").open, true);
  assert.equal(el("#self-test-cancel").disabled, false);
  assert.equal(el("#self-test-start").disabled, true);
  state.self_test.state = "cleanup"; renderSelfTest(state);
  assert.equal(el("#self-test-cancel").disabled, true);
}));


test("self-test presents retained throughput and unknown enums without obsolete request errors",
  () => withDocument((el, renderStatus) => {
  const state = snapshot();
  state.self_test = decodeSelfTestReport(selfTest(6));
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);
  assert.equal(el("#self-test-state").textContent, "Passed · 100%");
  assert.equal(el("#self-test-meter").dataset.result, "passed");
  assert.equal(el("#self-test-write-speed").textContent, "2.00 MiB/s");
  assert.equal(el("#self-test-read-speed").textContent, "4.00 MiB/s");
  assert.equal(el("#self-test-progress").value, 100);
  assert.equal(el("#self-test-message").textContent, "");
  state.self_test = decodeSelfTestReport(selfTest(99));
  renderSelfTest(state);
  assert.match(el("#self-test-state").textContent, /^Unknown state \(99\) · \d+%$/);
  assert.equal(el("#self-test-meter").dataset.result, "");
  assert.match(el("#self-test-message").textContent, /Unknown status values/);
  assert.equal(el("#self-test-start").disabled, true);
  state.self_test = { ...state.self_test, state: "failed", active: false };
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, true);
  el("#self-test-disclosure").open = false;
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);
}));


test("self-test view models fail closed without a fresh controllable report", () => {
  const model = deriveSelfTestViewModel({ connected: true }, 32);
  assert.equal(model.text["self-test-state"], "Unavailable");
  assert.equal(model.startDisabled, true);
  assert.equal(model.cancelDisabled, true);
});

