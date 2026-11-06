// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { BoardSession } from "./board/session.mjs";
import { HidTransport } from "./transport/hid.mjs";
import { BleTransport } from "./transport/ble.mjs";
import { createDashboardStore } from "./state/store.mjs";
import { createTimerScope } from "./timers.mjs";
import { mountDashboard } from "./ui/dashboard.mjs";

const timers = createTimerScope();
const store = createDashboardStore({});

function createSession(kind) {
  const transport = kind === "ble"
    ? new BleTransport(globalThis.isSecureContext ? navigator.bluetooth : undefined)
    : new HidTransport(globalThis.isSecureContext ? navigator.hid : undefined);
  return new BoardSession(transport, { timerScope: timers, store });
}

/** Browser facts injected into the UI, read live so they match `createSession`; app.mjs is the only global reader. */
const environment = {
  get secure() { return Boolean(globalThis.isSecureContext); },
  get hid() { return Boolean(navigator.hid); },
  get ble() { return Boolean(navigator.bluetooth); },
  get clipboard() { return navigator.clipboard ?? null; },
  now: () => Date.now(),
};

let session = createSession("usb");
const dashboard = mountDashboard({
  session, timers, environment,
  selectTransport(kind) {
    session.dispose();
    session = createSession(kind);
    return session;
  },
});
window.addEventListener?.("pagehide", () => {
  dashboard.dispose();
  session.dispose();
  timers.dispose();
}, { once: true });
window.addEventListener?.("pageshow", event => {
  // Cached pages have disposed sessions; reload preserves the journal without resuming stale device state.
  if (event.persisted) location.reload();
});
