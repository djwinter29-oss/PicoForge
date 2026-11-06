import assert from "node:assert/strict";
import test from "node:test";

import { createDashboardStore } from "../js/state/store.mjs";
import { dashboardReducer } from "../js/state/reducer.mjs";

test("dashboard reducer returns immutable snapshots and ignores unchanged patches", () => {
  const first = { connected: false, settings_dirty: false };
  const second = dashboardReducer(first, { type: "patch", changes: { connected: true } });
  assert.deepEqual(first, { connected: false, settings_dirty: false });
  assert.deepEqual(second, { connected: true, settings_dirty: false });
  assert.throws(() => dashboardReducer(second, { type: "invalid" }), /Unknown dashboard state action/);

  const notifications = [];
  const store = createDashboardStore(first, state => notifications.push(state));
  assert.deepEqual(store.dispatch({ type: "patch", changes: { connected: false } }), first);
  assert.equal(notifications.length, 1, "every dispatched action notifies the owner");
  const unsubscribe = store.subscribe(state => notifications.push(state));
  store.dispatch({ type: "patch", changes: { settings_dirty: true } });
  assert.equal(notifications.length, 3, "the owner and subscribers see each committed snapshot");
  assert.equal(notifications[1], store.getState());
  unsubscribe();
  store.dispatch({ type: "patch", changes: { connected: true } });
  assert.equal(notifications.length, 4);
});
