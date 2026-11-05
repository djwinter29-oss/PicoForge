import assert from "node:assert/strict";
import test from "node:test";
import { deriveOperationEligibility } from "../js/eligibility.mjs?v=48";

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
  self_test_command_pending: false,
  msc_command_pending: false,
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
    ["wireless busy", readyState(), { wirelessBusy: true }, [1, 1, 1, 0, 0, 0]],
    ["storage stale", readyState({ freshness: { 10: "stale", 11: "fresh" } }), {}, [0, 0, 0, 0, 0, 0]],
    ["self-test stale", readyState({ freshness: { 10: "fresh", 11: "stale" } }), {}, [0, 0, 0, 0, 0, 0]],
    ["MSC transition", readyState({ msc: { enabled: false, transition_pending: true } }), {}, [0, 0, 0, 0, 0, 0]],
    ["MSC command", readyState({ msc_command_pending: true }), {}, [0, 0, 0, 0, 0, 0]],
    ["self-test command", readyState({ self_test_command_pending: true }), {}, [0, 0, 0, 0, 0, 0]],
    // A running self-test keeps cancel available but blocks every storage-owning write.
    ["self-test running", readyState({ self_test: { valid_for_control: true, active: true } }), {},
      [0, 1, 0, 0, 0, 1]],
    ["MSC enabled", readyState({ msc: { enabled: true, transition_pending: false } }), {}, [0, 1, 1, 1, 0, 0]],
    ["outstanding", readyState(), { outstanding: {} }, [0, 1, 1, 1, 0, 1]],
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
  assert.equal(deriveOperationEligibility(readyState({ connected: false }), { outstanding: {} }).connectionLocked,
    true);
});
