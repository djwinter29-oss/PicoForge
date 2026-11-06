import assert from "node:assert/strict";
import test from "node:test";
import { canRun, deriveOperationEligibility } from "../web/js/state/selectors.mjs";

const readyState = (changes = {}) => ({
  link: "usb",
  connected: true,
  busy: false,
  outcome_uncertain: false,
  reboot_pending: false,
  hardware: { capabilities: 32 | 128 },
  freshness: { 10: "fresh", 11: "fresh" },
  self_test: { valid_for_control: true, active: false },
  msc: { enabled: false, transition_pending: false },
  pending_request: null,
  pending_wireless: null,
  pending_msc: null,
  pending_self_test: null,
  reconnecting: false,
  ...changes,
});

test("operation eligibility is one rule table for storage, settings, wireless and self-test gates", () => {
  assert.deepEqual(deriveOperationEligibility(readyState()), { blocked: false, storageFresh: true,
    storageReady: true, settingsReady: true, wirelessReady: true, selfTestReady: true, connectionLocked: true });

  const flags = ["blocked", "storageFresh", "storageReady", "settingsReady", "wirelessReady", "selfTestReady"];
  const cases = [
    ["disconnected", readyState({ connected: false }), {}, [1, 1, 1, 0, 0, 0]],
    ["busy", readyState({ busy: true }), {}, [1, 1, 1, 0, 0, 0]],
    ["uncertain", readyState({ outcome_uncertain: true }), {}, [1, 1, 1, 0, 0, 0]],
    ["reboot pending", readyState({ reboot_pending: true }), {}, [1, 1, 1, 0, 0, 0]],
    ["wireless busy", readyState({ wireless_busy: true }), {}, [1, 1, 1, 0, 0, 0]],
    ["command pending", readyState({ pending_request: { opcode: 1 } }), {}, [1, 1, 1, 0, 0, 0]],
    ["lifecycle pending", readyState({ pending_wireless: { operation: 9 } }), {}, [1, 1, 1, 0, 0, 0]],
    ["storage stale", readyState({ freshness: { 10: "stale", 11: "fresh" } }), {}, [0, 0, 0, 0, 0, 0]],
    ["self-test stale", readyState({ freshness: { 10: "fresh", 11: "stale" } }), {}, [0, 0, 0, 0, 0, 0]],
    ["MSC transition", readyState({ msc: { enabled: false, transition_pending: true } }), {}, [0, 0, 0, 0, 0, 0]],
    ["MSC command", readyState({ pending_msc: true }), {}, [1, 0, 0, 0, 0, 0]],
    ["self-test command", readyState({ pending_self_test: { sequence: 3 } }), {}, [1, 0, 0, 0, 0, 0]],
    // A running self-test keeps cancel available but blocks every storage-owning write.
    ["self-test running", readyState({ self_test: { valid_for_control: true, active: true } }), {},
      [0, 1, 0, 0, 0, 1]],
    ["MSC enabled", readyState({ msc: { enabled: true, transition_pending: false } }), {}, [0, 1, 1, 1, 0, 0]],
        ["Bluetooth", readyState({ link: "ble" }), {}, [0, 1, 1, 1, 0, 0]],
    ["Bluetooth MSC", readyState({ link: "ble", msc: { enabled: true, transition_pending: false } }), {},
      [0, 1, 1, 0, 0, 0]],
    ["no radio", readyState({ hardware: { capabilities: 32 } }), {}, [0, 1, 1, 1, 0, 1]],
    ["no self-test", readyState({ hardware: { capabilities: 128 }, freshness: { 10: "fresh", 11: "stale" } }), {},
      [0, 1, 1, 1, 1, 0]],
  ];
  for (const [name, state, options, expected] of cases) {
    const eligibility = deriveOperationEligibility(state, options);
    assert.deepEqual(flags.map(flag => Number(eligibility[flag])), expected, name);
  }
  assert.equal(deriveOperationEligibility(readyState({ connected: false })).connectionLocked, false);
  assert.equal(deriveOperationEligibility(readyState({ connected: false, pending_request: {} })).connectionLocked,
    true);
});

test("canRun is the one gate and names the first failing precondition per operation", () => {
  const ready = readyState({ hardware: { capabilities: 1 | 2 | 4 | 32 | 128 }, runtime: { led_available: true } });
  for (const operation of ["led", "reboot", "msc", "selfTest", "settings", "wireless"]) {
    assert.deepEqual(canRun(ready, operation), { allowed: true, reason: null }, operation);
  }
  const blocked = "Command unavailable or previous outcome uncertain";
  const cases = [
    ["led", { runtime: null }, "Fresh LED telemetry unavailable"],
    ["led", { hardware: { capabilities: 32 } }, "Unavailable capability"],
    ["led", { pending_msc: true }, blocked],
    ["reboot", { msc: { enabled: true, transition_pending: false } }, "Fresh idle firmware-owned storage required"],
    ["msc", { link: "ble" }, "This operation requires USB."],
    ["msc", { busy: true }, blocked],
    ["selfTest", { self_test: { valid_for_control: false } }, "Fresh self-test and firmware-owned storage required"],
    ["settings", { hardware: { capabilities: 32 } }, "Unavailable capability"],
    ["settings", { pending_request: { opcode: 1 } }, blocked],
    ["wireless", { wireless_busy: true },
      "Wireless configuration requires fresh idle USB storage, with sharing disabled."],
  ];
  for (const [operation, changes, reason] of cases) {
    assert.deepEqual(canRun({ ...ready, ...changes }, operation), { allowed: false, reason },
      `${operation} ${JSON.stringify(changes)}`);
  }
});
