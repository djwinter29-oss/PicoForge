import assert from "node:assert/strict";
import test from "node:test";
import { deriveOperationEligibility } from "../js/eligibility.mjs";

const readyState = () => ({
  connected: true,
  busy: false,
  outcome_uncertain: false,
  reboot_pending: false,
  hardware: { capabilities: 32 },
  freshness: { 10: "fresh", 11: "fresh" },
  self_test: { valid_for_control: true, active: false },
  msc: { enabled: false, transition_pending: false },
  self_test_command_pending: false,
  msc_command_pending: false,
  reconnecting: false,
});
const options = { bluetooth: false, bleClient: false, accessMode: null, outstanding: null, wirelessBusy: false };

test("operation eligibility centralizes connection, freshness, and transport gates", () => {
  const ready = deriveOperationEligibility(readyState(), options);
  assert.deepEqual(ready, { blocked: false, storageReady: true, settingsReady: true,
    connectionLocked: true, bleAccessChoicePending: false });

  for (const change of [
    { connected: false },
    { busy: true },
    { outcome_uncertain: true },
    { reboot_pending: true },
  ]) {
    const eligibility = deriveOperationEligibility({ ...readyState(), ...change }, options);
    assert.equal(eligibility.blocked, true);
    assert.equal(eligibility.settingsReady, false);
  }
  for (const update of [
    state => { state.freshness[10] = "stale"; },
    state => { state.freshness[11] = "stale"; },
    state => { state.msc.transition_pending = true; },
    state => { state.self_test.active = true; },
    state => { state.self_test_command_pending = true; },
    state => { state.msc_command_pending = true; },
  ]) {
    const state = readyState();
    update(state);
    const eligibility = deriveOperationEligibility(state, options);
    assert.equal(eligibility.storageReady, false);
    assert.equal(eligibility.settingsReady, false);
  }

  assert.equal(deriveOperationEligibility(readyState(), { ...options, wirelessBusy: true }).blocked, true);
  assert.equal(deriveOperationEligibility(readyState(), { ...options, bluetooth: true }).settingsReady, false);
  assert.equal(deriveOperationEligibility(readyState(), { ...options, outstanding: {} }).connectionLocked, true);
  assert.equal(deriveOperationEligibility({ ...readyState(), connected: true }, {
    ...options, bleClient: true, accessMode: null,
  }).bleAccessChoicePending, true);
});

test("boards without self-test capability do not require self-test freshness", () => {
  const state = { ...readyState(), hardware: { capabilities: 0 }, freshness: { 10: "fresh", 11: "stale" } };
  assert.equal(deriveOperationEligibility(state, options).storageReady, true);
});
