/**
 * @file Single source of operation readiness. BoardSession guards and every view gate derive from these functions
 * so buttons and device writes cannot disagree.
 */
import { CAP } from "../protocol/constants.mjs";

const BLOCKED = "Command unavailable or previous outcome uncertain";
const USB_ONLY = "This operation requires USB.";

/** @brief True while a control request or an accepted wireless lifecycle has no known outcome. */
export const commandPending = state => Boolean(state.pending_request || state.pending_wireless);
/** @brief True while a requested MSC ownership change has not been observed. */
export const mscPending = state => state.pending_msc != null;
/** @brief True while a self-test start/cancel has not changed the reported sequence. */
export const selfTestPending = state => state.pending_self_test != null;

/**
 * @brief Whether the single command queue may accept a request, ignoring the wireless workflow lock (the session
 * admits only the workflow that holds it).
 */
export function commandBlocked(state) {
  return Boolean(state.busy || commandPending(state) || mscPending(state) || selfTestPending(state)
    || state.outcome_uncertain || state.reboot_pending || !state.connected);
}

/**
 * @brief Derive operation readiness from a domain snapshot.
 * @param state Domain state snapshot.
 * @return Readiness flags: storageFresh (fresh storage/self-test reports, nothing pending), storageReady
 *   (fresh and no self-test running), and per-operation gates.
 */
export function deriveOperationEligibility(state) {
  const bluetooth = state.link === "ble";
  const caps = state.hardware?.capabilities ?? 0;
  const storageFresh = state.freshness?.[10] === "fresh" && Boolean(state.msc) && !state.msc.transition_pending
    && !mscPending(state) && !selfTestPending(state)
    && (!(caps & CAP.SELF_TEST) || state.freshness[11] === "fresh" && Boolean(state.self_test?.valid_for_control));
  const storageReady = storageFresh && !state.self_test?.active;
  const blocked = commandBlocked(state) || Boolean(state.wireless_busy);
  return {
    blocked,
    storageFresh,
    storageReady,
    settingsReady: !blocked && storageReady && (!bluetooth || !state.msc.enabled),
    wirelessReady: !blocked && !bluetooth && storageReady && !state.msc.enabled
      && Boolean(caps & CAP.WIRELESS),
    selfTestReady: !blocked && !bluetooth && storageFresh && Boolean(caps & CAP.SELF_TEST) && !state.msc.enabled,
    connectionLocked: Boolean(state.connected || state.reconnecting || commandPending(state)
      || state.outcome_uncertain),
  };
}

/**
 * @brief Per-operation precondition: return the first failing reason, or null. Operation-specific arguments
 * (LED level, MSC direction, self-test action, forced save) are checked by the session method itself.
 */
const RULES = {
  led: (state, e, caps) => !state.runtime?.led_available ? "Fresh LED telemetry unavailable"
    : !(caps & CAP.LED) ? "Unavailable capability" : e.blocked ? BLOCKED : null,
  reboot: (state, e, caps) => !e.storageReady || state.msc.enabled ? "Fresh idle firmware-owned storage required"
    : !(caps & CAP.REBOOT) ? "Unavailable capability" : e.blocked ? BLOCKED : null,
  msc: (state, e) => !e.storageReady ? "Fresh idle storage status required"
    : state.link === "ble" ? USB_ONLY : e.blocked ? BLOCKED : null,
  selfTest: (state, e, caps) => !e.storageFresh || state.msc.enabled || !state.self_test?.valid_for_control
    ? "Fresh self-test and firmware-owned storage required" : !(caps & CAP.SELF_TEST) ? "Unavailable capability"
      : state.link === "ble" ? USB_ONLY : e.blocked ? BLOCKED : null,
  settings: (state, e, caps) => !e.storageReady ? "Fresh idle firmware-owned storage required before saving."
    : !(caps & CAP.SETTINGS) ? "Unavailable capability"
      : !e.settingsReady ? e.blocked ? BLOCKED : USB_ONLY : null,
  wireless: (state, e) => e.wirelessReady ? null
    : "Wireless configuration requires fresh idle USB storage, with sharing disabled.",
};

/**
 * @brief The one readiness gate for user operations, shared by BoardSession methods and UI disabling.
 * @param state Domain state snapshot.
 * @param {"led" | "reboot" | "msc" | "selfTest" | "settings" | "wireless"} operation
 * @return {{allowed: boolean, reason: string | null}}
 */
export function canRun(state, operation) {
  const reason = RULES[operation](state, deriveOperationEligibility(state), state.hardware?.capabilities ?? 0);
  return { allowed: reason === null, reason };
}
