// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { deriveControlsViewModel } from "../../../web/js/ui/sections/controls.mjs";
import { deriveOperationEligibility } from "../../../web/js/state/selectors.mjs";
import { snapshot } from "../../support/view.mjs";

test("wireless changes explain shared-storage Save blocking without blocking ordinary forced saves", () => {
  const base = { ...snapshot(), settings_supported: true,
    hardware: { ...snapshot().hardware, capabilities: 183 } };
  for (const shared of [true, false]) {
    for (const wirelessDirty of [true, false]) {
      const state = { ...base, msc: { enabled: shared, transition_pending: false } };
      const model = deriveControlsViewModel(state, {
        eligibility: deriveOperationEligibility(state), formBusy: false, settingsMessage: "Earlier save message",
        bleName: null, settingsDirty: true, wirelessDirty, wirelessSaveBlockReason: null,
      }, { secure: true, hid: true, ble: true });
      assert.equal(model.disabled["settings-save"], shared && wirelessDirty);
      if (shared && wirelessDirty) {
        assert.match(model.titles["settings-save"], /safely eject.*Return to Pico/);
        assert.equal(model.text["settings-note"], model.titles["settings-save"]);
      } else assert.equal(model.titles["settings-save"], "");
    }
  }
});
