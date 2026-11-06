import assert from "node:assert/strict";
import test from "node:test";
import { operationState } from "../../src/js/operation-state.mjs";

const ready = {
  connected: true, runtime: { led_available: true }, msc: { enabled: false }, memory: { initialized: true },
  self_test: { active: false, state: "idle" }, filesystem_format: "exfat",
};

test("one pure policy handles every local gate without changing board facts", () => {
  const facts = structuredClone(ready);
  assert.equal(operationState(ready, {}, 32).canStartSelfTest, true);
  for (const flag of ["uncertain", "rebootPending", "resyncing", "saving", "wirelessBusy", "statusFailed"]) {
    const state = operationState(ready, { [flag]: true }, 32);
    assert.equal(state.canStartSelfTest, false, flag);
    assert.equal(state.canSetLed, false, flag);
    assert.equal(state.canChangeMsc, false, flag);
    assert.equal(operationState({ ...ready, self_test: { active: true } }, { [flag]: true }, 32)
      .canCancelSelfTest, false, flag);
  }
  for (const flag of ["selfTestInFlight", "selfTestPending", "mscInFlight", "mscPending"]) {
    const state = operationState(ready, { [flag]: true }, 32);
    assert.equal(state.canStartSelfTest, false, flag);
    assert.equal(state.canChangeMsc, false, flag);
    assert.equal(state.canSetLed, true, "unrelated LED access remains supported");
  }
  assert.deepEqual(ready, facts);
});

test("benchmark policy enforces actual size bounds, ownership, and initialized filesystem", () => {
  for (const size of [NaN, Infinity, -1, 0, 1.5, 1025])
    assert.equal(operationState(ready, {}, size).canStartSelfTest, false, String(size));
  for (const size of [1, 1024]) assert.equal(operationState(ready, {}, size).canStartSelfTest, true);
  for (const change of [{ connected: false }, { memory: { initialized: false } }, { filesystem_format: "unknown" },
    { msc: { enabled: true } }, { msc: { transition_pending: true } }, { self_test: null }])
    assert.equal(operationState({ ...ready, ...change }, {}, 32).canStartSelfTest, false);
  assert.equal(operationState({ ...ready, self_test: { active: true, state: "writing" } }).canCancelSelfTest, true);
  assert.equal(operationState({ ...ready, self_test: { active: true, state: "cleanup" } }).canCancelSelfTest, false);
});
