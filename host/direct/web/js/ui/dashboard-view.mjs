// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { renderDevice } from "./sections/device.mjs";
import { renderWirelessStatus } from "./sections/wireless.mjs";
import { createStorageSection } from "./sections/storage.mjs";
import { renderSelfTest } from "./sections/self-test.mjs";
import { renderClockOptions } from "./sections/controls.mjs";

export { renderControls } from "./sections/controls.mjs";

export function createDashboardView() {
  const renderStorage = createStorageSection();
  return (state, dashboard) => {
    renderClockOptions(state);
    renderDevice(state, dashboard);
    renderWirelessStatus(state);
    renderStorage(state);
    renderSelfTest(state);
  };
}
