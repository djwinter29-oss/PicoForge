// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
/**
 * @file Minimal observable store; the domain store is owned by BoardSession and the UI store by the dashboard.
 */

import { dashboardReducer, uiReducer } from "./reducer.mjs";
/** @typedef {import("./reducer.mjs").DashboardState} DashboardState */

/**
 * @brief Create a store around a reducer.
 * @template State
 * @param {(state: State, action: import("./reducer.mjs").StateAction<State>) => State} reducer
 * @param {State} initial
 */
export function createStore(reducer, initial) {
  let state = initial;
  const listeners = new Set();
  return {
    getState: () => state,
    /** @param {import("./reducer.mjs").StateAction<State>} action */
    dispatch(action) {
      state = reducer(state, action);
      for (const listener of listeners) listener(state);
      return state;
    },
    /** @param {(state: State) => void} listener */
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

/** @param {DashboardState} [initial] */
export const createDashboardStore = (initial = {}) => createStore(dashboardReducer, initial);

/** @brief Initial dashboard-only state; reset whenever the board disconnects. */
/** @type {Readonly<import("./reducer.mjs").UiState>} */
export const UI_INITIAL = Object.freeze({
  settings_dirty: false, wireless_form_busy: false, configuration_force_save: false, settings_message: null,
  action_error: null,
});

export const createUiStore = () => createStore(uiReducer, { ...UI_INITIAL });
