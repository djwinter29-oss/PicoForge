import { mountDashboard } from "../../web/js/ui/dashboard.mjs";
import { createTimerScope } from "../../web/js/timers.mjs";
import { fakeConnection, storage, selfTest, telemetry, emit, patchState } from "../support/fixtures.mjs";

const { client, device } = fakeConnection({ capabilities: 128 | 55 });
const timers = createTimerScope();
const ready = async () => {
  await client.connect();
  emit(device, 10, storage());
  emit(device, 11, selfTest());
  await client.readWireless();
  await Promise.resolve();
};
await ready();
const dashboard = mountDashboard({
  session: client, selectTransport: () => client, timers,
  environment: { secure: true, hid: true, ble: false, now: Date.now, clipboard: null },
});
window.directFixture = {
  client, ready, patch: changes => patchState(client, changes),
  telemetry: uptime => {
    const report = telemetry();
    new DataView(report.buffer).setBigUint64(4, BigInt(uptime), true);
    emit(device, 3, report);
  },
  dispose() { dashboard.dispose(); client.dispose(); timers.dispose(); },
};
