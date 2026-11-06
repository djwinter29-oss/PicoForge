// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { UI_INITIAL } from "../../web/js/state/store.mjs";
import { dashboardReducer, uiReducer } from "../../web/js/state/reducer.mjs";

test("domain and UI reducers return immutable snapshots and reject each other's keys", () => {
  const first = { connected: false, busy: false };
  const second = dashboardReducer(first, { type: "command/started", changes: { busy: true } });
  assert.deepEqual(first, { connected: false, busy: false });
  assert.deepEqual(second, { connected: false, busy: true });
  assert.throws(() => dashboardReducer(second, { type: "patch", changes: { busy: false } }),
    /Unknown domain event patch/, "the domain snapshot changes only through named events");
  assert.throws(() => dashboardReducer(second, { type: "command/started", changes: { connected: true } }),
    /Unknown command\/started keys: connected/, "each event may change only its own facts");
  assert.throws(() => dashboardReducer(second, { type: "connection/changed", changes: { settings_dirty: true } }),
    /Unknown connection\/changed keys: settings_dirty/, "UI facts cannot enter the board snapshot");
  assert.throws(() => uiReducer(UI_INITIAL, { type: "patch", changes: { connected: true } }),
    /Unknown state keys: connected/, "board facts cannot enter the UI snapshot");
  assert.throws(() => dashboardReducer(second, { type: "replace", state: { wireless_form_busy: false } }));
});
