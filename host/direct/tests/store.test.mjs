import assert from "node:assert/strict";
import test from "node:test";

import { createDashboardStore, createUiStore, UI_INITIAL } from "../web/js/state/store.mjs";
import { dashboardReducer, uiReducer } from "../web/js/state/reducer.mjs";

test("domain and UI reducers return immutable snapshots and reject each other's keys", () => {
  const first = { connected: false, busy: false };
  const second = dashboardReducer(first, { type: "patch", changes: { connected: true } });
  assert.deepEqual(first, { connected: false, busy: false });
  assert.deepEqual(second, { connected: true, busy: false });
  assert.throws(() => dashboardReducer(second, { type: "invalid" }), /Unknown state action/);
  assert.throws(() => dashboardReducer(second, { type: "patch", changes: { settings_dirty: true } }),
    /Unknown state keys: settings_dirty/, "UI facts cannot enter the board snapshot");
  assert.throws(() => uiReducer(UI_INITIAL, { type: "patch", changes: { connected: true } }),
    /Unknown state keys: connected/, "board facts cannot enter the UI snapshot");
  assert.throws(() => dashboardReducer(second, { type: "replace", state: { wireless_form_busy: false } }));

  const store = createDashboardStore(first);
  const notifications = [];
  const unsubscribe = store.subscribe(state => notifications.push(state));
  store.dispatch({ type: "patch", changes: { busy: true } });
  assert.equal(notifications.length, 1, "subscribers see each committed snapshot");
  assert.equal(notifications[0], store.getState());
  unsubscribe();
  store.dispatch({ type: "patch", changes: { connected: true } });
  assert.equal(notifications.length, 1);

  const ui = createUiStore();
  assert.deepEqual(ui.getState(), UI_INITIAL);
  ui.dispatch({ type: "patch", changes: { settings_dirty: true } });
  assert.equal(UI_INITIAL.settings_dirty, false, "the initial UI snapshot is never mutated");
});
