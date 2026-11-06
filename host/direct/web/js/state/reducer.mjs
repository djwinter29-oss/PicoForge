// @ts-check
/**
 * @file Allowlisted patch reducers for the domain (board) snapshot and the separate UI snapshot.
 */

/**
 * @brief Board facts written only by BoardSession. Pending facts live here once; selectors derive the flags.
 * `pending_request` is the persisted outstanding control request, `pending_wireless` the accepted scan/test
 * lifecycle, `pending_msc` the requested MSC ownership (null when none), `pending_self_test` the self-test
 * sequence observed before a start/cancel command (null when none).
 */
export const DOMAIN_KEYS = Object.freeze([
  "link", "connected", "reconnecting", "busy", "error", "hardware", "settings", "settings_supported", "runtime",
  "memory", "filesystem_format", "msc", "self_test", "wireless", "wireless_error", "wireless_poll_error",
  "recovery_message", "recovered_opcode", "freshness", "reboot_pending", "outcome_uncertain", "wireless_busy",
  "pending_request", "pending_wireless", "pending_msc", "pending_self_test",
]);

/**
 * @brief Dashboard-only presentation facts written only by the UI layer. `action_error` is the last failed user
 * action; board errors stay in the domain `error`.
 */
export const UI_KEYS = Object.freeze([
  "settings_dirty", "wireless_form_busy", "configuration_force_save", "settings_message", "action_error",
]);

/**
 * @typedef {Record<string, any>} DashboardState
 * @typedef {{type: "patch", changes: Partial<DashboardState>} | {type: "replace", state: DashboardState}}
 *   DashboardAction
 */

/**
 * @brief Build a reducer that accepts only the given keys, so UI facts cannot leak into board state or vice versa.
 * ponytail: one generic `patch` action instead of a named action per transition. The session is the only domain
 * writer (enforced by architecture.test.mjs) and the allowlist rejects foreign keys; move to named actions if a
 * second writer or time-travel debugging ever appears.
 * @param {readonly string[]} keys Allowed state keys.
 * @return {(state: DashboardState, action: DashboardAction) => DashboardState}
 */
export function allowlistReducer(keys) {
  const check = object => {
    const unknown = Object.keys(object).filter(key => !keys.includes(key));
    if (unknown.length) throw new Error(`Unknown state keys: ${unknown.join(", ")}`);
  };
  return (state, action) => {
    if (action.type === "replace") { check(action.state); return action.state; }
    if (action.type === "patch") { check(action.changes); return { ...state, ...action.changes }; }
    throw new Error("Unknown state action");
  };
}

export const dashboardReducer = allowlistReducer(DOMAIN_KEYS);
export const uiReducer = allowlistReducer(UI_KEYS);
