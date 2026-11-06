// @ts-check
/**
 * @typedef {Record<string, unknown> & {
 *   freshness?: Record<number, string>, busy?: boolean, connected?: boolean, command_pending?: boolean,
 *   wireless_busy?: boolean, wireless_form_busy?: boolean, settings_dirty?: boolean,
 *   configuration_force_save?: boolean, outcome_uncertain?: boolean, reboot_pending?: boolean
 * }} DashboardState
 */
/**
 * @typedef {{type: "patch", changes: Partial<DashboardState>} | {type: "replace", state: DashboardState}}
 *   DashboardAction
 */

/** @param {DashboardState} state @param {DashboardAction} action */
export function dashboardReducer(state, action) {
  if (action.type === "replace") return action.state;
  if (action.type === "patch") return { ...state, ...action.changes };
  throw new Error("Unknown dashboard state action");
}
