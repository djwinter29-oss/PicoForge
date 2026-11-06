// @ts-check
/**
 * @file Minimal observable store; the domain store is owned by BoardSession and the UI store by the dashboard.
 */

import { dashboardReducer, uiReducer } from "./reducer.mjs";
/** @typedef {import("./reducer.mjs").DashboardState} DashboardState */
/** @typedef {import("./reducer.mjs").DashboardAction} DashboardAction */

/**
 * @brief Create a store around a reducer.
 * @param {(state: DashboardState, action: DashboardAction) => DashboardState} reducer
 * @param {DashboardState} initial
 */
export function createStore(reducer, initial) {
  let state = initial;
  const listeners = new Set();
  return {
    getState: () => state,
    /** @param {DashboardAction} action */
    dispatch(action) {
      state = reducer(state, action);
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

/** @param {DashboardState} [initial] */
export const createDashboardStore = (initial = {}) => createStore(dashboardReducer, initial);

/** @brief Initial dashboard-only state; reset whenever the board disconnects. */
export const UI_INITIAL = Object.freeze({
  settings_dirty: false, wireless_form_busy: false, configuration_force_save: false, settings_message: null,
  action_error: null,
});

export const createUiStore = () => createStore(uiReducer, { ...UI_INITIAL });
