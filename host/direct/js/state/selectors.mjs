/**
 * Single source of operation readiness for the Direct dashboard. The client's guards and every view gate derive
 * from this function so buttons and device writes cannot disagree.
 */
import { CAP } from "../protocol/constants.mjs";

/**
 * @brief Derive operation readiness from a client snapshot.
 * @param state Client state snapshot.
 * @return Readiness flags: storageFresh (fresh storage/self-test reports, nothing pending), storageReady
 *   (fresh and no self-test running), and per-operation gates.
 */
export function deriveOperationEligibility(state) {
  const bluetooth = state.link === "ble";
  const caps = state.hardware?.capabilities ?? 0;
  const storageFresh = state.freshness?.[10] === "fresh" && Boolean(state.msc) && !state.msc.transition_pending
    && !state.msc_command_pending && !state.self_test_command_pending
    && (!(caps & CAP.SELF_TEST) || state.freshness[11] === "fresh" && Boolean(state.self_test?.valid_for_control));
  const storageReady = storageFresh && !state.self_test?.active;
  const blocked = Boolean(state.busy || state.command_pending || state.wireless_busy || state.wireless_form_busy
    || state.outcome_uncertain || state.reboot_pending || !state.connected);
  return {
    blocked,
    storageFresh,
    storageReady,
    settingsReady: !blocked && storageReady && (!bluetooth || !state.msc.enabled),
    wirelessReady: !blocked && !bluetooth && storageReady && !state.msc.enabled
      && Boolean(caps & CAP.WIRELESS),
    selfTestReady: !blocked && !bluetooth && storageFresh && Boolean(caps & CAP.SELF_TEST) && !state.msc.enabled,
    connectionLocked: Boolean(state.connected || state.reconnecting || state.command_pending || state.outcome_uncertain),
  };
}
