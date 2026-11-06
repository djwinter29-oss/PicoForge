// @ts-check
/**
 * @file Allowlisted patch reducers for the domain (board) snapshot and the separate UI snapshot.
 */

/**
 * @brief Board facts written only by the board modules. `phase` is the connection lifecycle phase
 * (board/connection.mjs); `connected` and `reconnecting` are its projections. Pending facts live here once and
 * selectors derive the flags: `pending_request` is the persisted outstanding control request, `pending_wireless` the
 * accepted scan/test lifecycle, `pending_msc` the requested MSC ownership (null when none), `pending_self_test` the
 * self-test sequence observed before a start/cancel command (null when none).
 */
export const DOMAIN_KEYS = Object.freeze([
  "link", "phase", "connected", "reconnecting", "busy", "error", "hardware", "settings", "settings_supported",
  "runtime", "memory", "filesystem_format", "msc", "self_test", "wireless", "wireless_error", "wireless_poll_error",
  "recovery_message", "recovered_opcode", "freshness", "reboot_pending", "outcome_uncertain", "wireless_busy",
  "pending_request", "pending_wireless", "pending_msc", "pending_self_test",
]);

/** @brief Per-connection facts reset when a connection opens or closes; pending request facts survive. */
export const CONNECTION_FACTS = Object.freeze([
  "hardware", "settings", "settings_supported", "runtime", "memory", "filesystem_format", "msc", "self_test",
  "wireless", "wireless_error", "wireless_poll_error", "recovery_message", "recovered_opcode", "freshness",
  "pending_self_test", "reboot_pending",
]);

const PHASE = ["phase", "connected", "reconnecting"];
const REPORTS = ["runtime", "memory", "filesystem_format", "msc", "self_test", "wireless", "wireless_error",
  "freshness"];
/**
 * @brief Named domain events and the only keys each may change. Phase keys appear only in `connection/*` events,
 * which board/connection.mjs alone emits (architecture.test.mjs).
 */
export const DOMAIN_EVENTS = Object.freeze({
  "connection/changed": [...PHASE, "busy", "error"],
  "connection/opened": [...PHASE, ...CONNECTION_FACTS, "busy", "error", "outcome_uncertain"],
  "connection/closed": [...PHASE, ...CONNECTION_FACTS, "busy", "error", "outcome_uncertain", "pending_msc"],
  "command/started": ["busy", "pending_request", "pending_msc", "error", "recovery_message", "recovered_opcode"],
  "command/retired": ["pending_request", "pending_wireless", "pending_msc"],
  "command/finished": ["busy", "outcome_uncertain", "error", "settings", "reboot_pending", "pending_self_test"],
  "lifecycle/settled": ["pending_wireless"],
  "recovery/finished": ["outcome_uncertain", "error", "recovered_opcode", "recovery_message"],
  "report/received": [...REPORTS, "wireless_poll_error", "pending_msc", "pending_self_test"],
  "report/expired": REPORTS,
  "report/failed": ["error", "wireless_poll_error", "wireless", "wireless_error"],
  "wireless/locked": ["wireless_busy"],
  "session/failed": ["error", "outcome_uncertain"],
});

/**
 * @brief Dashboard-only presentation facts written only by the UI layer. `action_error` is the last failed user
 * action; board errors stay in the domain `error`.
 */
export const UI_KEYS = Object.freeze([
  "settings_dirty", "wireless_form_busy", "configuration_force_save", "settings_message", "action_error",
]);

/**
 * @typedef {Record<string, any>} DashboardState
 * @typedef {{type: string, changes?: Partial<DashboardState>, state?: DashboardState}} DashboardAction
 *   `replace` carries `state`; `patch` (UI store) and named domain events carry `changes`.
 */

const checkKeys = (object, keys, label) => {
  const unknown = Object.keys(object).filter(key => !keys.includes(key));
  if (unknown.length) throw new Error(`Unknown ${label} keys: ${unknown.join(", ")}`);
};

/**
 * @brief Build a reducer that accepts only the given keys, so UI facts cannot leak into board state or vice versa.
 * ponytail: the UI store keeps one generic `patch` action; it has one writer (the dashboard) and no transitions.
 * @param {readonly string[]} keys Allowed state keys.
 * @return {(state: DashboardState, action: DashboardAction) => DashboardState}
 */
export function allowlistReducer(keys) {
  return (state, action) => {
    if (action.type === "replace") { checkKeys(action.state, keys, "state"); return action.state; }
    if (action.type === "patch") { checkKeys(action.changes, keys, "state"); return { ...state, ...action.changes }; }
    throw new Error("Unknown state action");
  };
}

/**
 * @brief Domain reducer: `replace` (session start) or a named event from DOMAIN_EVENTS changing only its keys.
 * @type {(state: DashboardState, action: DashboardAction) => DashboardState}
 */
export function dashboardReducer(state, action) {
  if (action.type === "replace") { checkKeys(action.state, DOMAIN_KEYS, "state"); return action.state; }
  const keys = Object.hasOwn(DOMAIN_EVENTS, action.type) ? DOMAIN_EVENTS[action.type] : null;
  if (!keys || !("changes" in action)) throw new Error(`Unknown domain event ${action.type}`);
  checkKeys(action.changes, keys, action.type);
  return { ...state, ...action.changes };
}

export const uiReducer = allowlistReducer(UI_KEYS);
