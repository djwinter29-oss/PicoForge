// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import {
  commandEffects, commandSettled, createCommandState, observeCommandStatus, operationState, unresolvedCommand,
} from "../../src/js/operation-state.mjs";

const ready = {
  connected: true, runtime: { led_available: true }, msc: { enabled: false }, memory: { initialized: true },
  self_test: { active: false, state: "idle" }, filesystem_format: "exfat",
};

test("one Save projection supplies eligibility and the same reason for all draft and board gates", () => {
  const facts = { ...ready, hardware: { capabilities: 128 | 2 }, settings_supported: true,
    settings: { system_clock_mhz: 200, sdcard_profile: 0, radios: 1 }, storage_stale: false,
    wireless: { stale: false } };
  const draft = { settingsDirty: true };
  assert.equal(operationState(facts, draft).canSaveSettings, true);
  assert.equal(operationState(facts, draft).saveBlockReason, null);
  for (const [changes, activity, reason] of [
    [{ connected: false }, {}, /connection/],
    [{ settings_supported: false }, {}, /Boot settings unavailable/],
    [{ hardware: { capabilities: 128 } }, {}, /does not support/],
    [{}, { saving: true }, /pending operation/],
    [{}, { uncertain: true }, /recovery/],
    [{ storage_stale: true }, {}, /fresh storage/],
    [{}, { selfTestPending: true }, /benchmark/],
    [{ wireless: null }, { radiosDirty: true }, /fresh wireless/],
    [{}, { wirelessBlockReason: "Retest credentials." }, /Retest credentials/],
    [{ msc: { enabled: true } }, { wirelessDirty: true }, /safely eject.*Return to Pico/],
    [{}, { settingsDirty: false }, /No unsaved/],
  ]) {
    const state = operationState({ ...facts, ...changes }, { ...draft, ...activity });
    assert.equal(state.canSaveSettings, false);
    assert.match(state.saveBlockReason, reason);
  }
  assert.equal(operationState({ ...facts, wireless: null }, draft).canSaveSettings, true);
  assert.equal(operationState({ ...facts, msc: { enabled: true } }, draft).canSaveSettings, true,
    "ordinary boot settings keep the explicit forced-save path");
});

test("radio writes require fresh wireless facts while unrelated settings preserve saved radios", () => {
  const facts = { ...ready, hardware: { capabilities: 128 | 2 }, settings_supported: true,
    settings: { system_clock_mhz: 200, sdcard_profile: 0, radios: 1 },
    storage_stale: false, wireless: { stale: false } };
  assert.equal(operationState(facts, { radiosDirty: true, settingsDirty: true }).canSaveSettings, true);
  assert.equal(operationState(facts).canProvisionWireless, true);
  for (const changes of [{ wireless: null }, { wireless: { stale: true } }, { wireless_error: "read failed" }]) {
    const state = operationState({ ...facts, ...changes }, { radiosDirty: true, settingsDirty: true });
    assert.equal(state.wirelessStatusReady, false);
    assert.equal(state.canSaveSettings, false);
    assert.equal(state.canProvisionWireless, false);
    assert.match(state.wirelessBlockReason, /Waiting for fresh wireless configuration/);
    assert.equal(operationState({ ...facts, ...changes }, { settingsDirty: true }).canSaveSettings, true);
    assert.equal(state.canSetLed, true, "missing wireless facts must not block unrelated writes");
  }
  for (const activity of [{ saving: true }, { uncertain: true }, { mscPending: true }])
    assert.equal(operationState(facts, activity).canProvisionWireless, false);
  assert.equal(operationState({ ...facts, msc: { enabled: true } }).canProvisionWireless, false);
  assert.equal(operationState({ ...facts, hardware: { capabilities: 0 } }).canProvisionWireless, false);
});

test("missing and unrelated results require a matching reconnect and fresh board facts", () => {
  for (const opcode of [3, 4]) {
    const facts = { ...ready, self_test: { sequence: 4 } };
    for (const result of [null, { opcode: 1, status: "completed" }]) {
      const command = unresolvedCommand(result, opcode === 3, opcode === 4, null);
      assert.equal(command.opcode, opcode);
      assert.equal(commandSettled(command, facts), false);
      const disconnected = { ...command, disconnected: true };
      assert.equal(commandSettled(disconnected, { connected: false }), false);
      assert.equal(commandSettled(disconnected, facts), true);
      assert.equal(commandSettled(disconnected, { ...facts, connected: "true" }), false);
      for (const flag of ["telemetry_stale", "storage_stale", "command_outcome_uncertain", "outcome_uncertain"])
        assert.equal(commandSettled(disconnected, { ...facts, [flag]: true }), false, flag);
      assert.equal(commandSettled(disconnected, { connected: true }), false);
      assert.equal(unresolvedCommand(null, opcode === 3, opcode === 4, disconnected).disconnected, true);
      assert.equal(unresolvedCommand(null, false, false, disconnected).opcode, opcode,
        "readback clearing a pending marker must not erase the reconnect requirement");
    }
    for (const status of ["completed", "failed", "busy", "invalid", "unsupported"])
      assert.equal(unresolvedCommand({ opcode, status }, opcode === 3, opcode === 4, null), null);
  }
  assert.equal(unresolvedCommand(null, false, false, null), null);
  const msc = { kind: "msc", enabled: true, message: "uncertain" };
  assert.equal(commandSettled(msc, ready), false);
  assert.equal(commandSettled(msc, { ...ready, connected: false, msc: { enabled: true } }), false);
  assert.equal(commandSettled(msc, { ...ready, msc: { enabled: true } }), true);
  assert.equal(commandSettled(msc, { ...ready, msc: { enabled: true }, storage_stale: true }), false);
  assert.equal(commandSettled(msc, { ...ready, msc: { enabled: true, transition_pending: true } }), false);
  assert.equal(commandSettled(msc, { ...ready, msc: { enabled: false, request_rejected: true } }), true);
});

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
  for (const connected of [undefined, null, 0, "", "false"])
    assert.equal(commandSettled(command({ kind: "reconnect" }), { connected }), false);
  for (const reboot_pending of [undefined, null, 0, 1, "", "true"])
    assert.equal(commandSettled(command({ kind: "reconnect" }), { connected: true, reboot_pending }), false);
  assert.equal(commandSettled(command({ kind: "reconnect" }), { connected: false }), true);
  assert.equal(commandSettled(command({ kind: "reconnect" }), { ...facts, reboot_pending: true }), true);
  assert.equal(commandSettled(command({ kind: "led", on: true }), facts), true);
  assert.equal(commandSettled(command({ kind: "led", on: false }), facts), false);
  assert.equal(commandSettled(command({ kind: "led", on: false }), {}), false);
  assert.equal(commandSettled(command({ kind: "settings", expected: settings }), facts), true);
  for (const key of Object.keys(settings)) {
    assert.equal(commandSettled(command({ kind: "settings", expected: { ...settings, [key]: 99 } }), facts), false);
  }
  assert.equal(commandSettled(command({ kind: "self-test" }), facts), false);
  assert.equal(commandSettled(command({ kind: "self-test" }), {}), false);
});

test("storage admission projects its own blocking reason without blocking unrelated LED access", () => {
  const facts = { ...ready, hardware: { capabilities: 32 }, self_test_stale: false };
  assert.equal(operationState(facts).storageBlockReason, null);
  for (const [changes, activity, reason] of [
    [{ storage_stale: true }, {}, /fresh storage ownership/],
    [{ msc: null }, {}, /fresh storage ownership/],
    [{ self_test: null }, {}, /fresh benchmark status/],
    [{ self_test_stale: true }, {}, /fresh benchmark status/],
    [{}, { selfTestPending: true }, /updated benchmark status/],
    [{ self_test: { active: true } }, {}, /finish or cancel/],
    [{}, { mscPending: true }, /USB storage transition/],
  ]) {
    const projection = operationState({ ...facts, ...changes }, activity);
    assert.match(projection.storageBlockReason, reason);
    assert.equal(projection.canChangeMsc, false);
    assert.equal(projection.canSetLed, true);
  }
});

test("periodic benchmark reports release observation fences, never unknown delivery", () => {
  const state = createCommandState();
  state.uncertainCommand = { kind: "self-test", message: "uncertain" };
  state.selfTestPendingSequence = 254;
  for (const sequence of [255, 0, 1]) {
    const snapshot = { ...ready, self_test: { sequence, active: false, state: "idle" } };
    assert.equal(observeCommandStatus(state, snapshot, false), null);
    assert.equal(state.selfTestPendingSequence, null, "a fresh report satisfies only the observation fence");
    assert.equal(state.uncertainCommand.kind, "self-test");
    assert.equal(unresolvedCommand(null, false, false, state.uncertainCommand).opcode, 4,
      "recovery identity outlives the observation fence");
  }
  const independent = createCommandState();
  assert.equal(independent.uncertainCommand, null);
  state.selfTestPendingSequence = 1;
  observeCommandStatus(state, { ...ready, self_test: { sequence: 2 } }, true);
  assert.equal(state.selfTestPendingSequence, 1, "reports during delivery cannot release its fence");
});

test("storage-dependent admission requires fresh benchmark facts only on supporting hardware", () => {
  const capable = { ...ready, hardware: { capabilities: 32 }, self_test_stale: false };
  assert.equal(operationState(capable, {}, 32).canStartSelfTest, true);
  for (const change of [{ self_test: null }, { self_test_stale: true }, { self_test_stale: undefined },
    { storage_stale: true }]) {
    const facts = { ...capable, ...change };
    const state = operationState(facts, {}, 32);
    assert.equal(state.canChangeMsc, false);
    assert.equal(state.canStartSelfTest, false);
    assert.equal(state.storageCommandPending, true);
    assert.equal(state.canSetLed, true);
  }
  assert.equal(operationState({ ...ready, self_test: null, self_test_stale: true }).canChangeMsc, true,
    "firmware without benchmark support does not need benchmark reports");
  const state = createCommandState();
  state.uncertainCommand = { kind: "reconnect-status", opcode: 4, disconnected: true, message: "reconnect" };
  observeCommandStatus(state, { ...capable, self_test: { sequence: 9 }, self_test_stale: true }, false);
  assert.notEqual(state.uncertainCommand, null, "stale reconnect reports cannot settle benchmark recovery");
});

test("confirmed benchmark delivery requires a post-command baseline then a newer report", () => {
  const state = createCommandState();
  state.selfTestPendingSequence = "awaiting-status";
  observeCommandStatus(state, { connected: false, self_test: { sequence: 254 } }, false);
  observeCommandStatus(state, { ...ready, self_test_stale: true, self_test: { sequence: 254 } }, false);
  observeCommandStatus(state, { ...ready, self_test: { sequence: 254 } }, true);
  assert.equal(state.selfTestPendingSequence, "awaiting-status");
  observeCommandStatus(state, { ...ready, self_test: { sequence: 255 } }, false);
  assert.equal(state.selfTestPendingSequence, 255);
  observeCommandStatus(state, { ...ready, self_test: { sequence: 255 } }, false);
  assert.equal(state.selfTestPendingSequence, 255);
  observeCommandStatus(state, { ...ready, self_test: { sequence: 0 } }, false);
  assert.equal(state.selfTestPendingSequence, null);
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
