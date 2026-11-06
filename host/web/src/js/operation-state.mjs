// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
/**
 * @typedef {object} OperationSnapshot
 * @property {boolean} [connected]
 * @property {boolean} [outcome_uncertain]
 * @property {boolean} [command_outcome_uncertain]
 * @property {boolean} [command_resync_supported]
 * @property {boolean} [wireless_cleanup_required]
 * @property {boolean} [reboot_pending]
 * @property {{led_available?: boolean, led_on?: boolean}} [runtime]
 * @property {{transition_pending?: boolean, enabled?: boolean}} [msc]
 * @property {{initialized?: boolean}} [memory]
 * @property {{active?: boolean, state?: string, sequence?: number}} [self_test]
 * @property {import("./dashboard-api.mjs").BootSettings | null} [settings]
 * @property {boolean} [self_test_command_pending]
 * @property {boolean} [msc_command_pending]
 * @property {boolean} [busy]
 * @property {string | null} [filesystem_format]
 *
 * @typedef {object} Activity
 * @property {boolean} [uncertain]
 * @property {boolean} [rebootPending]
 * @property {boolean} [resyncing]
 * @property {boolean} [saving]
 * @property {boolean} [wirelessBusy]
 * @property {boolean} [wirelessInFlight]
 * @property {boolean} [statusFailed]
 * @property {boolean} [mscInFlight]
 * @property {boolean} [mscPending]
 * @property {boolean} [selfTestInFlight]
 * @property {boolean} [selfTestPending]
 * @property {boolean} [ledInFlight]
 * @property {boolean} [scanning]
 *
 * @typedef {{kind: "reconnect" | "resync"} | {kind: "led", on: boolean}
 *   | {kind: "settings", expected: import("./dashboard-api.mjs").BootSettings}
 *   | {kind: "self-test", sequence: number}} Recovery
 * @typedef {Recovery & {message: string}} UncertainCommand
 */

/** Fresh report evidence may settle readback-based uncertainty, but never a resync-only obligation.
 * @param {UncertainCommand | null} command
 * @param {OperationSnapshot} snapshot
 */
export function commandSettled(command, snapshot) {
  switch (command?.kind) {
    case "reconnect": return !snapshot.connected || Boolean(snapshot.reboot_pending);
    case "led": return snapshot.runtime?.led_on === command.on;
    case "settings": return snapshot.settings?.system_clock_mhz === command.expected.system_clock_mhz
      && snapshot.settings?.sdcard_profile === command.expected.sdcard_profile
      && snapshot.settings?.radios === command.expected.radios;
    case "self-test": return Number.isInteger(snapshot.self_test?.sequence)
      && snapshot.self_test?.sequence !== command.sequence;
    default: return false;
  }
}

/** Terminal acknowledgement is not fresh ownership or benchmark progress.
 * @param {import("./dashboard-api.mjs").ControlResult} result
 */
export function commandEffects(result) {
  const completed = result.status === "completed";
  const reason = result.error && result.error !== "none" ? `: ${result.error}` : "";
  const detail = result.detail ? ` (detail ${result.detail})` : "";
  return {
    releaseMsc: !completed && result.opcode === 3,
    releaseSelfTest: !completed && result.opcode === 4,
    reboot: completed && (result.opcode === 2 || result.opcode === 5),
    wifiCancel: result.opcode === 10,
    completed,
    failure: completed ? null
      : `Command ${result.opcode} ${result.status}${reason}${detail}. Retry explicitly when the board is ready.`,
  };
}

/**
 * One admission policy for renderers and command handlers; local activity never changes reported board facts.
 * @param {OperationSnapshot | null} snapshot
 * @param {Activity} [activity]
 * @param {number} [sizeMib]
 */
export function operationState(snapshot, activity = {}, sizeMib = NaN) {
  const outcomeUncertain = Boolean(activity.uncertain || snapshot?.outcome_uncertain || snapshot?.command_outcome_uncertain);
  const rebootPending = Boolean(activity.rebootPending || snapshot?.reboot_pending);
  const cleanupRequired = Boolean(snapshot?.wireless_cleanup_required);
  const mscPending = Boolean(activity.mscInFlight || activity.mscPending || snapshot?.msc_command_pending);
  const selfTestPending = Boolean(activity.selfTestInFlight || activity.selfTestPending || snapshot?.self_test_command_pending);
  const storageCommandPending = mscPending || selfTestPending
    || Boolean(snapshot?.self_test?.active || snapshot?.msc?.transition_pending);
  const busy = Boolean(activity.resyncing || activity.saving || activity.wirelessBusy || rebootPending
    || outcomeUncertain || cleanupRequired || activity.statusFailed || snapshot?.busy);
  const mounted = ["fat12", "fat16", "fat32", "exfat"].includes(snapshot?.filesystem_format ?? "");
  const test = snapshot?.self_test;
  const validSize = Number.isInteger(sizeMib) && sizeMib >= 1 && sizeMib <= 1024;
  const testReady = snapshot?.connected && snapshot.memory?.initialized && mounted && test && !snapshot.msc?.enabled;
  /** @type {"resync" | "cleanup" | null} */
  const recovery = outcomeUncertain ? snapshot?.command_resync_supported ? "resync" : null
    : cleanupRequired ? "cleanup" : null;
  return {
    outcomeUncertain, rebootPending, mscPending, selfTestPending, storageCommandPending, busy,
    recovery,
    canRecover: Boolean(recovery && snapshot?.connected && !activity.resyncing && !activity.statusFailed
      && !activity.saving && !activity.wirelessInFlight && !activity.ledInFlight
      && !activity.mscInFlight && !activity.selfTestInFlight),
    settingsBusy: Boolean(activity.resyncing || activity.saving || activity.wirelessInFlight || rebootPending
      || outcomeUncertain || cleanupRequired || activity.statusFailed),
    canSetLed: !busy && Boolean(snapshot?.connected && snapshot.runtime?.led_available) && !activity.ledInFlight,
    canChangeMsc: !busy && !storageCommandPending && Boolean(snapshot?.connected && snapshot.msc),
    canStartSelfTest: Boolean(testReady && !test.active && !selfTestPending && !busy && !mscPending
      && !snapshot?.msc?.transition_pending && validSize),
    canCancelSelfTest: Boolean(snapshot?.connected && test?.active && test.state !== "cleanup"
      && !selfTestPending && !busy && !mscPending),
    canEditSelfTest: Boolean(snapshot?.connected && !test?.active && !selfTestPending && !mscPending),
    boardSelectionBusy: Boolean(activity.resyncing || activity.scanning || activity.saving || activity.wirelessBusy
      || activity.ledInFlight || activity.mscInFlight || activity.selfTestInFlight),
  };
}

/** @typedef {ReturnType<typeof operationState>} OperationState */
