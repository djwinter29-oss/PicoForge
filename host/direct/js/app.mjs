import { BoardSession } from "./board/session.mjs";
import { HidTransport } from "./transport/hid.mjs";
import { BleTransport } from "./transport/ble.mjs";
import { createDashboardStore } from "./state/store.mjs";
import { createTimerScope } from "./timers.mjs";
import { mountDashboard } from "./ui/dashboard.mjs";

const timers = createTimerScope();
const store = createDashboardStore({});
let dashboard;

function createSession(kind) {
  const transport = kind === "ble"
    ? new BleTransport(globalThis.isSecureContext ? navigator.bluetooth : undefined)
    : new HidTransport(globalThis.isSecureContext ? navigator.hid : undefined);
  return new BoardSession(transport, () => dashboard?.refresh(), { timerScope: timers, store });
}

let session = createSession("usb");
dashboard = mountDashboard({
  session, timers,
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
