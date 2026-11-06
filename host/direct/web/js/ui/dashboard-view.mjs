import { renderDevice } from "./sections/device.mjs";
import { renderWirelessStatus } from "./sections/wireless.mjs";
import { createStorageSection } from "./sections/storage.mjs";
import { renderSelfTest } from "./sections/self-test.mjs";
import { renderClockOptions } from "./sections/controls.mjs";

export { renderControls } from "./sections/controls.mjs";

export function createDashboardView() {
  const renderStorage = createStorageSection();
  return state => {
    renderClockOptions(state);
    renderDevice(state);
    renderWirelessStatus(state);
    renderStorage(state);
    renderSelfTest(state);
  };
}
