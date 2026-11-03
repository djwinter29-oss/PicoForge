export function deriveOperationEligibility(state, {
  bluetooth, bleClient, accessMode, outstanding, wirelessBusy,
}) {
  const selfTestReady = !(state.hardware?.capabilities & 32)
    || (state.freshness[11] === "fresh" && state.self_test?.valid_for_control);
  const storageReady = state.freshness[10] === "fresh" && selfTestReady
    && !state.msc?.transition_pending && !state.self_test?.active
    && !state.self_test_command_pending && !state.msc_command_pending;
  const blocked = state.busy || state.outcome_uncertain || state.reboot_pending || !state.connected || wirelessBusy;

  return {
    blocked,
    storageReady,
    settingsReady: !blocked && storageReady && !bluetooth,
    connectionLocked: state.connected || state.reconnecting || outstanding || state.outcome_uncertain,
    bleAccessChoicePending: bleClient && state.connected && accessMode === null,
  };
}
