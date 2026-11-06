// @ts-check
/**
 * @file Reducer-backed dashboard snapshot shared by the device session and UI.
 */

import { dashboardReducer } from "./reducer.mjs";
/** @typedef {import("./reducer.mjs").DashboardState} DashboardState */
/** @typedef {import("./reducer.mjs").DashboardAction} DashboardAction */

/**
 * @param {DashboardState} initial
 * @param {(state: DashboardState) => void} [onChange]
 */
export function createDashboardStore(initial, onChange = () => {}) {
  let state = initial;
  const listeners = new Set();
  return {
    getState: () => state,
    /** @param {DashboardAction} action */
    dispatch(action) {
      const next = dashboardReducer(state, action);
      state = next;
      onChange(state);
      for (const listener of listeners) listener(state);
      return state;
    },
    /** @param {(state: DashboardState) => void} listener */
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}
