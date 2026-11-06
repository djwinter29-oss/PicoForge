// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { createDashboardStore, createUiStore, UI_INITIAL } from "../../web/js/state/store.mjs";

test("stores notify subscribers and preserve the initial UI snapshot", () => {
  const first = { connected: false, busy: false };
  const store = createDashboardStore(first);
  const notifications = [];
  const unsubscribe = store.subscribe(state => notifications.push(state));
  store.dispatch({ type: "command/started", changes: { busy: true } });
  assert.equal(notifications.length, 1, "subscribers see each committed snapshot");
  assert.equal(notifications[0], store.getState());
  unsubscribe();
  store.dispatch({ type: "command/finished", changes: { busy: false } });
  assert.equal(notifications.length, 1);

  const ui = createUiStore();
  assert.deepEqual(ui.getState(), UI_INITIAL);
  ui.dispatch({ type: "patch", changes: { settings_dirty: true } });
  assert.equal(UI_INITIAL.settings_dirty, false, "the initial UI snapshot is never mutated");
});
