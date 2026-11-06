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
 * @property {boolean} [telemetry_stale]
 * @property {boolean} [storage_stale]
 * @property {boolean} [self_test_stale]
 * @property {{capabilities?: number} | null} [hardware]
 * @property {{led_available?: boolean, led_on?: boolean} | null} [runtime]
 * @property {{transition_pending?: boolean, enabled?: boolean, request_rejected?: boolean} | null} [msc]
 * @property {{initialized?: boolean} | null} [memory]
 * @property {{active?: boolean, state?: string, sequence?: number} | null} [self_test]
 * @property {(import("./dashboard-api.mjs").BootSettings & {tcp_cli_enabled?: boolean}) | null} [settings]
 * @property {boolean} [settings_supported]
 * @property {string | null} [settings_message]
 * @property {boolean} [self_test_command_pending]
 * @property {boolean} [msc_command_pending]
 * @property {boolean} [busy]
 * @property {{stale?: boolean} | null} [wireless]
 * @property {string | null} [wireless_error]
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
 * @property {boolean} [radiosDirty]
 * @property {boolean} [settingsDirty]
 * @property {boolean} [wirelessDirty]
 * @property {string | null} [wirelessBlockReason]
 *
 * @typedef {{kind: "reconnect" | "resync"} | {kind: "led", on: boolean}
 *   | {kind: "settings", expected: import("./dashboard-api.mjs").BootSettings}
 *   | {kind: "self-test"} | {kind: "msc", enabled: boolean}
 *   | {kind: "reconnect-status", opcode: 3 | 4, disconnected: boolean}} Recovery
 * @typedef {Recovery & {message: string}} UncertainCommand
 */

/** Fresh report evidence may settle readback-based uncertainty, but never a resync-only obligation.
 * @param {UncertainCommand | null} command
 * @param {OperationSnapshot} snapshot
 */
export function commandSettled(command, snapshot) {
  switch (command?.kind) {
    case "reconnect": return snapshot.connected === false || snapshot.reboot_pending === true;
    case "led": return snapshot.runtime?.led_on === command.on;
    case "settings": return snapshot.settings?.system_clock_mhz === command.expected.system_clock_mhz
      && snapshot.settings?.sdcard_profile === command.expected.sdcard_profile
      && snapshot.settings?.radios === command.expected.radios;
    case "msc": return Boolean(snapshot.connected && !snapshot.storage_stale && !snapshot.msc?.transition_pending
      && (snapshot.msc?.enabled === command.enabled || snapshot.msc?.request_rejected));
    case "reconnect-status": return Boolean(command.disconnected && snapshot.connected === true
      && !snapshot.telemetry_stale && !snapshot.storage_stale
      && !snapshot.command_outcome_uncertain && !snapshot.outcome_uncertain
      && (command.opcode === 3 ? typeof snapshot.msc?.enabled === "boolean"
        : !snapshot.self_test_stale && Number.isInteger(snapshot.self_test?.sequence)));
    default: return false;
  }
}

/** A clean host without the pending command's result is not proof of non-delivery.
 * @param {import("./dashboard-api.mjs").ControlResult | null} result
 * @param {boolean} mscPending
 * @param {boolean} selfTestPending
 * @param {UncertainCommand | null} command
 * @returns {UncertainCommand | null}
 */
export function unresolvedCommand(result, mscPending, selfTestPending, command) {
  const opcode = command?.kind === "reconnect-status" ? command.opcode
    : command?.kind === "self-test" ? 4 : mscPending ? 3 : selfTestPending ? 4 : null;
  if (opcode === null || result?.opcode === opcode) return null;
  return {
    kind: "reconnect-status", opcode,
    disconnected: command?.kind === "reconnect-status" && command.disconnected,
    message: "No matching command result is available. Disconnect and reconnect this board, then wait for fresh "
      + "status before retrying explicitly. The command was not resent.",
  };
}

/**
 * @typedef {object} BoardCommandState
 * @property {UncertainCommand | null} uncertainCommand
 * @property {string | null} mscError
 * @property {string | null} ledError
 * @property {number | "awaiting-status" | null} selfTestPendingSequence Post-command report fence, not a command ID.
 * @property {boolean | null} mscPendingEnabled
 * @property {"requested" | "observed" | "disconnected" | null} rebootPending
 * @property {{opcode: number, message: string} | null} commandFailure
 */

/** @returns {BoardCommandState} */
export function createCommandState() {
  return {
    uncertainCommand: null, mscError: null, ledError: null, selfTestPendingSequence: null,
    mscPendingEnabled: null, rebootPending: null, commandFailure: null,
  };
}

/** Update board-owned lifecycle state from a status poll, returning the settled recovery reason.
 * @param {BoardCommandState} state
 * @param {OperationSnapshot} snapshot
 * @param {boolean} selfTestInFlight
 * @returns {UncertainCommand | null}
 */
export function observeCommandStatus(state, snapshot, selfTestInFlight) {
  const command = state.uncertainCommand;
  if (command?.kind === "reconnect-status" && snapshot.connected === false) command.disconnected = true;
  if (state.rebootPending !== null) {
    if (snapshot.connected === false) state.rebootPending = "disconnected";
    else if (snapshot.reboot_pending) state.rebootPending = "observed";
    else if (snapshot.connected === true && state.rebootPending !== "requested") state.rebootPending = null;
  }
  const settled = commandSettled(command, snapshot) ? command : null;
  if (settled) {
    if (settled.kind === "reconnect-status") {
      if (settled.opcode === 3) {
        state.mscPendingEnabled = null;
        state.mscError = null;
      } else state.selfTestPendingSequence = null;
    }
    state.uncertainCommand = null;
    state.ledError = null;
  }
  if (snapshot.connected && !snapshot.storage_stale && snapshot.msc && !snapshot.msc.transition_pending
    && (snapshot.msc.enabled === state.mscPendingEnabled || snapshot.msc.request_rejected)) {
    state.mscPendingEnabled = null;
    state.mscError = null;
  }
  const sequence = snapshot.self_test?.sequence;
  if (!selfTestInFlight && snapshot.connected === true && !snapshot.self_test_stale
    && typeof sequence === "number" && Number.isInteger(sequence)) {
    if (state.selfTestPendingSequence === "awaiting-status")
      state.selfTestPendingSequence = sequence;
    else if (sequence !== state.selfTestPendingSequence) state.selfTestPendingSequence = null;
  }
  return settled;
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
  const wirelessStatusReady = Boolean(snapshot?.wireless && !snapshot.wireless.stale && !snapshot.wireless_error);
  const wirelessBlockReason = snapshot?.connected && (snapshot.hardware?.capabilities ?? 0) & 128
    && !wirelessStatusReady ? "Waiting for fresh wireless configuration before editing radios." : null;
  const mscPending = Boolean(activity.mscInFlight || activity.mscPending || snapshot?.msc_command_pending);
  const selfTestPending = Boolean(activity.selfTestInFlight || activity.selfTestPending || snapshot?.self_test_command_pending);
  const storageStatusReady = Boolean(snapshot?.msc && !snapshot.storage_stale
    && (!((snapshot.hardware?.capabilities ?? 0) & 32) || snapshot.self_test && snapshot.self_test_stale === false));
  const storageCommandPending = !storageStatusReady || mscPending || selfTestPending
    || Boolean(snapshot?.self_test?.active || snapshot?.msc?.transition_pending);
  const storageBlockReason = !snapshot?.connected ? null
    : !snapshot.msc || snapshot.storage_stale ? "Waiting for fresh storage ownership status."
      : (snapshot.hardware?.capabilities ?? 0) & 32 && (!snapshot.self_test || snapshot.self_test_stale !== false)
        ? "Waiting for fresh benchmark status before changing storage or rebooting."
        : mscPending || snapshot.msc.transition_pending ? "Wait for the USB storage transition to finish."
          : selfTestPending ? "Waiting for updated benchmark status after the command."
            : snapshot.self_test?.active ? "Wait for the benchmark to finish or cancel it before changing storage."
              : null;
  const busy = Boolean(activity.resyncing || activity.saving || activity.wirelessBusy || rebootPending
    || outcomeUncertain || cleanupRequired || activity.statusFailed || snapshot?.busy);
  const mounted = ["fat12", "fat16", "fat32", "exfat"].includes(snapshot?.filesystem_format ?? "");
  const test = snapshot?.self_test;
  const validSize = Number.isInteger(sizeMib) && sizeMib >= 1 && sizeMib <= 1024;
  const testReady = snapshot?.connected && snapshot.memory?.initialized && mounted && test && !snapshot.msc?.enabled;
  /** @type {"resync" | "cleanup" | null} */
  const recovery = outcomeUncertain ? snapshot?.command_resync_supported ? "resync" : null
    : cleanupRequired ? "cleanup" : null;
  const canProvisionWireless = Boolean(snapshot?.connected && (snapshot.hardware?.capabilities ?? 0) & 128
    && wirelessStatusReady && !busy && !storageCommandPending
    && snapshot?.storage_stale === false && !snapshot.msc?.enabled);
  const saveBlockReason = !snapshot?.connected ? "Waiting for a PicoForge connection."
    : !snapshot.settings_supported || !snapshot.settings
      ? snapshot.settings_message || "Boot settings unavailable; update device firmware."
      : !((snapshot.hardware?.capabilities ?? 0) & 2) ? "Firmware does not support saving boot settings."
        : busy ? "Finish the pending operation or recovery before saving settings."
          : storageBlockReason
            ?? (activity.radiosDirty && !wirelessStatusReady ? wirelessBlockReason : null)
            ?? activity.wirelessBlockReason
            ?? (activity.wirelessDirty && snapshot.msc?.enabled
              ? "Wireless changes cannot be saved while USB storage is shared. Stop host file activity, safely eject, "
                + "then use Return to Pico before saving."
              : activity.wirelessDirty && !canProvisionWireless ? "Wait for fresh wireless and storage status before saving."
                : !activity.settingsDirty && !activity.wirelessDirty ? "No unsaved settings changes." : null);
  return {
    outcomeUncertain, rebootPending, mscPending, selfTestPending, storageCommandPending, storageStatusReady,
    storageBlockReason, wirelessStatusReady, wirelessBlockReason, busy,
    canProvisionWireless, canSaveSettings: saveBlockReason === null, saveBlockReason,
    recovery,
    canRecover: Boolean(recovery && snapshot?.connected && !activity.resyncing && !activity.statusFailed
      && !activity.saving && !activity.wirelessInFlight && !activity.ledInFlight
      && !activity.mscInFlight && !activity.selfTestInFlight),
    settingsBusy: Boolean(activity.resyncing || activity.saving || activity.wirelessInFlight || rebootPending
      || outcomeUncertain || cleanupRequired || activity.statusFailed),
    canSetLed: !busy && Boolean(snapshot?.connected && snapshot.runtime?.led_available) && !activity.ledInFlight,
    canChangeMsc: !busy && !storageCommandPending && Boolean(snapshot?.connected && snapshot.msc),
    canStartSelfTest: Boolean(storageStatusReady && !snapshot?.self_test_stale
      && testReady && !test.active && !selfTestPending && !busy && !mscPending
      && !snapshot?.msc?.transition_pending && validSize),
    canCancelSelfTest: Boolean(storageStatusReady && !snapshot?.self_test_stale
      && snapshot?.connected && test?.active && test.state !== "cleanup"
      && !selfTestPending && !busy && !mscPending),
    canEditSelfTest: Boolean(snapshot?.connected && !test?.active && !selfTestPending && !mscPending),
    boardSelectionBusy: Boolean(activity.resyncing || activity.scanning || activity.saving || activity.wirelessBusy
      || activity.ledInFlight || activity.mscInFlight || activity.selfTestInFlight),
  };
}

/** @typedef {ReturnType<typeof operationState>} OperationState */
