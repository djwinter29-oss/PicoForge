// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { decodeIdentityReport, decodeConfigReport } from "../../../web/js/protocol/codec.mjs";
import { identity, config } from "../../support/fixtures.mjs";
import { withDocument, snapshot } from "../../support/view.mjs";
import { deriveDashboardViewModel } from "../../../web/js/ui/sections/status-model.mjs";
import { deriveDeviceViewModel } from "../../../web/js/ui/sections/device.mjs";

test("board pin tables are reused until the displayed configuration changes", () => withDocument((el, renderStatus) => {
  const state = snapshot();
  renderStatus(state);
  const uartRows = el("#board-uart").children;
  const storageRows = el("#board-storage-pins").children;
  assert.equal(el("#board-uart").replaceCount, 1);
  assert.equal(el("#board-storage-pins").replaceCount, 1);

  renderStatus(snapshot());
  assert.strictEqual(el("#board-uart").children, uartRows);
  assert.strictEqual(el("#board-storage-pins").children, storageRows);
  assert.equal(el("#board-uart").replaceCount, 1);

  state.settings = { ...state.settings, reset_cause: 2 };
  renderStatus(state);
  assert.equal(el("#board-uart").replaceCount, 2);
  assert.equal(el("#board-storage-pins").replaceCount, 2);
}));

test("identity RAM stays build accounting while stale inputs show no current values", () =>
  withDocument((el, renderStatus) => {
  const state = snapshot();
  state.runtime = { uptime_ms: 9007199254740993n, system_clock_hz: 200000000,
    cpu_usage_core0_basis_points: 1234, cpu_usage_core1_basis_points: null, led_available: true, led_on: true };
  renderStatus(state);
  assert.equal(el("#ram-usage").textContent, "64.0 / 520 KiB (12.3%)");
  assert.equal(el("#board-model").textContent, "Pico 2 (RP2350)");
  assert.equal(el("#cpu-usage-core0").textContent, "12.3%");
  assert.equal(el("#cpu-usage-core1").textContent, "--");
  assert.match(el("#uptime").textContent, /104249991d/);
  assert.equal(el("#led-control")["aria-pressed"], "true");
  const rows = id => el(id).children.map(row => row.children.map(cell => cell.textContent));
  assert.deepEqual(rows("#board-uart"), [["TX", "RX"], ["GP0", "GP1"]]);
  assert.deepEqual(rows("#board-storage-pins"), [
    ["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3", "Card detect"],
    ["GP18", "GP19", "GP20", "GP21", "GP22", "GP23", "GP8"],
  ]);
  assert.deepEqual(el("#settings-clock").children.map(option => option.value), ["0", "200", "250"]);
  state.runtime = null; state.memory = null; state.msc = null; state.self_test = null;
  state.freshness = { 3: "stale", 10: "stale", 11: "stale" };
  renderStatus(state);
  assert.equal(el("#identity-clock").textContent, "--");
  assert.equal(el("#cpu-usage-core0").textContent, "--");
  assert.equal(el("#ram-usage").textContent, "64.0 / 520 KiB (12.3%)");
  assert.equal(el("#telemetry-freshness").textContent, "telemetry: stale");
  assert.equal(el("#storage-freshness").textContent, "storage: stale");
  assert.equal(el("#led-control")["aria-pressed"], "false");
}));

test("SPI pin table has two rows and keeps card detect after the four bus signals", () =>
  withDocument((el, renderStatus) => {
  const state = snapshot();
  state.hardware = decodeIdentityReport(identity(1 | 2 | 4 | 8 | 32));
  state.settings = decodeConfigReport(config("spi"), state.hardware);
  state.settings.card_detect_gpio = 255;
  renderStatus(state);
  assert.equal(el("#product-name").textContent, "PicoForge (SPI)");
  assert.equal(el("#board-storage-title").textContent, "Storage (SPI)");
  const rows = el("#board-storage-pins").children;
  assert.equal(rows.length, 2);
  assert.deepEqual(rows[0].children.map(cell => cell.textContent), ["CLK", "MOSI", "MISO", "CS", "Card detect"]);
  assert.deepEqual(rows[1].children.map(cell => cell.textContent), ["GP18", "GP19", "GP20", "GP21", "Unassigned"]);
  assert.ok(rows[0].children.every(cell => cell.scope === "col"));
}));

test("device rendering reuses the supplied dashboard presentation snapshot", () => {
  const state = { connected: true };
  const dashboard = deriveDashboardViewModel(state, true);
  const device = deriveDeviceViewModel(state, dashboard);
  assert.equal(device.eligibility, dashboard.eligibility, "render-local readiness is not derived again");
  assert.equal(device.eligibility.blocked, true, "the form lock survives section composition");
  assert.equal(device.connection_label, dashboard.connection_label);
});
