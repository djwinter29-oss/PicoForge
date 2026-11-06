// @ts-check
/**
 * @typedef {object} OperationSnapshot
 * @property {boolean} [connected]
 * @property {boolean} [outcome_uncertain]
 * @property {boolean} [command_outcome_uncertain]
 * @property {boolean} [reboot_pending]
 * @property {{led_available?: boolean}} [runtime]
 * @property {{transition_pending?: boolean, enabled?: boolean}} [msc]
 * @property {{initialized?: boolean}} [memory]
 * @property {{active?: boolean, state?: string}} [self_test]
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
 */

/**
 * One admission policy for renderers and command handlers; local activity never changes reported board facts.
 * @param {OperationSnapshot | null} snapshot
 * @param {Activity} [activity]
 * @param {number} [sizeMib]
 */
export function operationState(snapshot, activity = {}, sizeMib = NaN) {
  const outcomeUncertain = Boolean(activity.uncertain || snapshot?.outcome_uncertain || snapshot?.command_outcome_uncertain);
  const rebootPending = Boolean(activity.rebootPending || snapshot?.reboot_pending);
  const mscPending = Boolean(activity.mscInFlight || activity.mscPending || snapshot?.msc_command_pending);
  const selfTestPending = Boolean(activity.selfTestInFlight || activity.selfTestPending || snapshot?.self_test_command_pending);
  const storageCommandPending = mscPending || selfTestPending
    || Boolean(snapshot?.self_test?.active || snapshot?.msc?.transition_pending);
  const busy = Boolean(activity.resyncing || activity.saving || activity.wirelessBusy || rebootPending
    || outcomeUncertain || activity.statusFailed || snapshot?.busy);
  const mounted = ["fat12", "fat16", "fat32", "exfat"].includes(snapshot?.filesystem_format ?? "");
  const test = snapshot?.self_test;
  const validSize = Number.isInteger(sizeMib) && sizeMib >= 1 && sizeMib <= 1024;
  const testReady = snapshot?.connected && snapshot.memory?.initialized && mounted && test && !snapshot.msc?.enabled;
  return {
    outcomeUncertain, rebootPending, mscPending, selfTestPending, storageCommandPending, busy,
    settingsBusy: Boolean(activity.resyncing || activity.saving || activity.wirelessInFlight || rebootPending
      || outcomeUncertain || activity.statusFailed),
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
