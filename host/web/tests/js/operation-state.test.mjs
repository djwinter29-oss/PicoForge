import assert from "node:assert/strict";
import test from "node:test";
import { commandEffects, commandSettled, operationState } from "../../src/js/operation-state.mjs";

const ready = {
  connected: true, runtime: { led_available: true }, msc: { enabled: false }, memory: { initialized: true },
  self_test: { active: false, state: "idle" }, filesystem_format: "exfat",
};

test("terminal effects release only failed matching gates and preserve successful observation requirements", () => {
  for (let opcode = 1; opcode <= 13; opcode++) {
    for (const status of ["completed", "failed", "busy", "invalid", "unsupported"]) {
      const completed = status === "completed";
      const effects = commandEffects({ opcode, status });
      assert.equal(effects.releaseMsc, !completed && opcode === 3);
      assert.equal(effects.releaseSelfTest, !completed && opcode === 4);
      assert.equal(effects.reboot, completed && [2, 5].includes(opcode));
      assert.equal(effects.wifiCancel, opcode === 10);
      assert.equal(effects.completed, completed);
      if (completed) assert.equal(effects.failure, null);
      else assert.match(effects.failure, new RegExp(status));
    }
  }
  assert.match(commandEffects({ opcode: 4, status: "failed", error: "storage_busy", detail: 2 }).failure,
    /storage_busy \(detail 2\)/);
});

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

test("recovery projection gives local and server uncertainty the same explicit admission", () => {
  const facts = { ...ready, command_resync_supported: true };
  for (const [snapshot, activity] of [
    [facts, { uncertain: true }], [{ ...facts, command_outcome_uncertain: true }, {}],
    [{ ...facts, wireless_cleanup_required: true }, { uncertain: true }],
  ]) {
    assert.equal(operationState(snapshot, activity).recovery, "resync");
    assert.equal(operationState(snapshot, activity).canRecover, true);
    for (const flag of ["resyncing", "statusFailed", "saving", "wirelessInFlight", "ledInFlight",
      "mscInFlight", "selfTestInFlight"]) {
      assert.equal(operationState(snapshot, { ...activity, [flag]: true }).canRecover, false, flag);
    }
    assert.equal(operationState({ ...snapshot, connected: false }, activity).canRecover, false);
  }
  assert.equal(operationState({ ...facts, wireless_cleanup_required: true }).recovery, "cleanup");
  const unsupported = operationState(ready, { uncertain: true });
  assert.equal(unsupported.recovery, null);
  assert.equal(unsupported.canRecover, false);
  assert.equal(unsupported.canSetLed, false);
});

test("tagged uncertainty settles only on the matching fresh evidence", () => {
  const settings = { system_clock_mhz: 200, sdcard_profile: 0, radios: 3 };
  const facts = { ...ready, settings, runtime: { led_on: true }, self_test: { sequence: 4 } };
  const command = recovery => ({ ...recovery, message: "uncertain" });
  assert.equal(commandSettled(null, facts), false);
  assert.equal(commandSettled(command({ kind: "resync" }), facts), false);
  assert.equal(commandSettled(command({ kind: "reconnect" }), facts), false);
  assert.equal(commandSettled(command({ kind: "reconnect" }), { connected: false }), true);
  assert.equal(commandSettled(command({ kind: "reconnect" }), { ...facts, reboot_pending: true }), true);
  assert.equal(commandSettled(command({ kind: "led", on: true }), facts), true);
  assert.equal(commandSettled(command({ kind: "led", on: false }), facts), false);
  assert.equal(commandSettled(command({ kind: "led", on: false }), {}), false);
  assert.equal(commandSettled(command({ kind: "settings", expected: settings }), facts), true);
  for (const key of Object.keys(settings)) {
    assert.equal(commandSettled(command({ kind: "settings", expected: { ...settings, [key]: 99 } }), facts), false);
  }
  assert.equal(commandSettled(command({ kind: "self-test", sequence: 3 }), facts), true);
  assert.equal(commandSettled(command({ kind: "self-test", sequence: 4 }), facts), false);
  assert.equal(commandSettled(command({ kind: "self-test", sequence: 3 }), {}), false);
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

test("known wireless cleanup blocks writes without inventing transaction uncertainty or locking selection", () => {
  const state = operationState({ ...ready, wireless_cleanup_required: true }, {}, 32);
  assert.equal(state.outcomeUncertain, false);
  assert.equal(state.settingsBusy, true);
  assert.equal(state.canSetLed, false);
  assert.equal(state.canChangeMsc, false);
  assert.equal(state.canStartSelfTest, false);
  assert.equal(state.boardSelectionBusy, false);
});
