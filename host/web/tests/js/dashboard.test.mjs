// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import {
  normalizeStatus, renderStatus as renderServerStatus, renderSelfTest as renderServerSelfTest,
} from "../../src/js/dashboard.mjs";
import { operationState } from "../../src/js/operation-state.mjs";
import { statusResponse } from "../../src/js/dashboard-api.mjs";

function withDocument(run) {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", value: "0", open: false,
      valueAsNumber: 32, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({ children: [],
    append(...children) { this.children.push(...children); } }) };
  try { run(element); } finally { globalThis.document = saved; }
}

function snapshot() {
  return { connected: true,
    hardware: { product_name: "PicoForge", board_model: "Pico 2", mcu: "RP2350", capabilities: 55,
      serial_number: "0102030405060708", ram_total_bytes: 532480, ram_reserved_bytes: 65536 },
    settings: { system_clock_mhz: 200, sdcard_profile: 1, storage_gpios: [18, 19, 20, 21, 22, 23],
      monitor_uart_index: 0, monitor_uart_tx_gpio: 0, monitor_uart_rx_gpio: 1, card_detect_gpio: 8 },
    self_test: { state: "idle", active: false, volume_mounted: true },
    memory: { sample_valid: true, initialized: true, configured: true, presence_known: true, present: true },
    msc: { enabled: false, transition_pending: false }, filesystem_format: "exfat" };
}

test("presentation uses the supplied operation projection consistently without reconstructing admission", () =>
  withDocument(el => {
    const state = snapshot();
    const operations = { ...operationState(state), canSetLed: false, canChangeMsc: false,
      canSaveSettings: false, saveBlockReason: "Projected Save gate.", canRecover: false,
      recovery: "resync", canStartSelfTest: false, canCancelSelfTest: false, canEditSelfTest: false };
    renderServerStatus({ ...state, settings_dirty: true }, operations);
    for (const id of ["led-control", "msc-control", "settings-save", "command-resync",
      "self-test-start", "self-test-cancel", "self-test-size"]) assert.equal(el(`#${id}`).disabled, true);
    assert.equal(el("#settings-save").title, operations.saveBlockReason);
    assert.equal(el("#settings-note").textContent, operations.saveBlockReason);
    assert.equal(el("#command-resync").hidden, false);
  }));

test("every native benchmark state has a known display label", () => withDocument(el => {
  for (const [state, label] of Object.entries({
    idle: "Idle", queued: "Queued", preparing: "Preparing", writing: "Writing", reading: "Verifying",
    cleanup: "Cleanup", passed: "Passed", failed: "Failed", cancelled: "Cancelled",
  })) {
    renderServerSelfTest({ ...snapshot(), self_test: { state, progress_percent: 0 } });
    assert.equal(el("#self-test-state").textContent, `${label} · 0%`);
  }
}));

test("missing-card guidance uses reported board wiring, not a fixed GPIO", () => withDocument(el => {
  for (const gpio of [3, 8, 255, undefined]) {
    renderServerStatus({ ...snapshot(), board_configuration: { ...snapshot().settings, card_detect_gpio: gpio },
      memory: { configured: true, sample_valid: true, presence_known: true, present: false } });
    const note = el("#memory-note").textContent;
    if (gpio !== undefined && gpio !== 255) assert.match(note, new RegExp(`GP${gpio}\\b`));
    else assert.doesNotMatch(note, /GP\d/);
  }
}));

test("server dashboard shows live Wi-Fi and Bluetooth state without treating unavailable reports as disabled", () => withDocument(el => {
  for (const render of [renderServerStatus]) {
    const state = { ...snapshot(), hardware: { ...snapshot().hardware, capabilities: 128,
      serial_number: "892BA313F9BB56E1" } };
    for (const [wireless, expected] of [
      [{ wifi_enabled: false, ssid: "saved", wifi_state: "connected" }, "Disabled"],
      [{ wifi_enabled: true, ssid: "", wifi_state: "failed" }, "Disabled"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "connected", ipv4: "192.168.1.9" }, "HomeDev (192.168.1.9)"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "connecting" }, "HomeDev (connecting)"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "failed" }, "HomeDev (disconnected)"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "disabled" }, "HomeDev (disconnected)"],
      [{ wifi_enabled: true, ssid: "<script>", wifi_state: "connected", ipv4: "10.0.0.2" }, "<script> (10.0.0.2)"],
      [null, "--"],
    ]) {
      render({ ...state, wireless });
      assert.equal(el("#wifi-state").textContent, expected);
      assert.equal(el("#wifi-diagnostics").hidden, false);
    }
    for (const [wireless, expected] of [
      [{ ble_enabled: false, ble_key_configured: true, ble_state: "advertising" }, "Disabled"],
      [{ ble_enabled: true, ble_key_configured: false, ble_state: "off" }, "Off (no control key)"],
      [{ ble_enabled: true, ble_key_configured: true, ble_state: "off" }, "PicoForge-892BA313F9BB56E1 (starting)"],
      [{ ble_enabled: true, ble_key_configured: true, ble_state: "advertising" },
        "PicoForge-892BA313F9BB56E1 (advertising)"],
      [{ ble_enabled: true, ble_key_configured: true, ble_state: "connected" }, "PicoForge-892BA313F9BB56E1 (connected)"],
      [{ ble_enabled: true, ble_key_configured: false, ble_state: "connected" }, "PicoForge-892BA313F9BB56E1 (connected)"],
      [{ ble_enabled: true, ble_key_configured: false, ble_state: "advertising" },
        "PicoForge-892BA313F9BB56E1 (advertising)"],
      [null, "--"],
    ]) {
      render({ ...state, wireless });
      assert.equal(el("#ble-state").textContent, expected);
      assert.equal(el("#ble-diagnostics").hidden, false);
    }
    render({ ...state, hardware: { capabilities: 128 },
      wireless: { ble_enabled: true, ble_state: "connected" } });
    assert.equal(el("#ble-state").textContent, "PicoForge (connected)");
    render({ ...state, wireless_error: "Wi-Fi status read failed: disconnected" });
    assert.equal(el("#ble-state").textContent, "Unavailable");
    assert.equal(el("#wifi-state").textContent, "Unavailable");
    assert.match(el("#wifi-state").title, /read failed/);
    render({ ...state, connected: false });
    assert.equal(el("#wifi-state").textContent, "--");
    render({ ...state, hardware: { capabilities: 16 } });
    assert.equal(el("#wifi-diagnostics").hidden, true);
    assert.equal(el("#ble-diagnostics").hidden, true);
  }
}));

test("server dashboard opens the benchmark disclosure on active and failed transitions", () => withDocument(el => {
  const state = snapshot();
  renderServerSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);

  state.self_test = { ...state.self_test, state: "writing", active: true };
  renderServerSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, true);
  el("#self-test-disclosure").open = false;
  renderServerSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);

  state.self_test = { ...state.self_test, state: "failed", active: false };
  renderServerSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, true);
}));

test("server dashboard retains storage visibility through stale reports without showing cached readings", () =>
  withDocument(el => {
    for (const render of [renderServerStatus]) {
      render({ connected: false });
      const state = snapshot();
      state.memory.present = false;
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, true);
      assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
      state.memory = null;
      state.msc = null;
      state.storage_stale = true;
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, true, "A missing report must not reveal absent storage");
      assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
      state.memory = { sample_valid: true, presence_known: true, present: true, initialized: true, configured: true };
      state.storage_stale = false;

      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false, "Confirmed card insertion must update layout");
      state.memory = null;
      state.storage_stale = true;

      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(el("#memory-size").textContent, "--", "Stale readings must not be presented as live");

      render({ connected: false });
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false, "Disconnect must clear confirmed presence");
    }
  }));

test("status normalization reads only the shared nested shape", () => {
  const msc = { enabled: true, transition_pending: false };
  const status = normalizeStatus({ settings_supported: true, memory: { msc, filesystem_format: "fat32" },
    board_configuration: { system_clock_mhz: 250, sdcard_profile: 1, radios: 3, tcp_cli_enabled: true, led_gpio: 25 } });
  assert.deepEqual(status.msc, msc);
  assert.equal(status.filesystem_format, "fat32");
  assert.deepEqual(status.settings, { system_clock_mhz: 250, sdcard_profile: 1, radios: 3, tcp_cli_enabled: true });
  const stale = normalizeStatus({ settings_supported: false, memory: null, board_configuration: null });
  assert.equal(stale.msc, null);
  assert.equal(stale.filesystem_format, null);
  assert.equal(stale.settings, null);
});

test("diagnostic rendering distinguishes missing facts, explicit false and reported unassigned sentinels", () =>
  withDocument(el => {
    for (const cdc_open of [undefined, "bad", false, true]) {
      const result = statusResponse({
        connected: true, reboot_pending: false, command_outcome_uncertain: false, settings_supported: true,
        telemetry_stale: false, storage_stale: false, self_test_stale: false,
        hardware: { capabilities: 2, product_name: {} },
        runtime: { cdc_open },
        memory: { msc: { enabled: false, transition_pending: false } },
        board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0,
          monitor_uart_index: "bad", monitor_uart_tx_gpio: "bad", monitor_uart_rx_gpio: 255,
          storage_gpios: [0, 255], reset_cause: "bad" },
      });
      renderServerStatus(normalizeStatus(result));
      assert.equal(el("#cdc-state").textContent, cdc_open === true ? "Open" : cdc_open === false ? "Closed" : "--");
      assert.equal(el("#cdc-state").dataset.active, typeof cdc_open === "boolean" ? String(cdc_open) : undefined);
      assert.equal(el("#board-uart-title").textContent, "Monitor UART");
      assert.equal(el("#board-uart").children[1].children[0].textContent, "--");
      assert.equal(el("#board-uart").children[1].children[1].textContent, "Unassigned");
      assert.deepEqual(el("#board-storage-pins").children[1].children.map(cell => cell.textContent),
        ["GP0", "Unassigned", "--", "--", "--", "--", "--"]);
      assert.equal(el("#board-reset-cause").textContent, "--");
      assert.equal(el("#product-name").textContent, "--");
      assert.equal(el("#board-model").textContent, "--");
      assert.match(el("#status-message").textContent, /Unavailable diagnostic fields/);
    }
    for (const monitor_uart_index of [0, 1, 255]) {
      renderServerStatus({ ...snapshot(), board_configuration: { ...snapshot().settings, monitor_uart_index } });
      assert.equal(el("#board-uart-title").textContent,
        monitor_uart_index === 255 ? "Monitor UART" : `Monitor UART${monitor_uart_index}`);
    }
    renderServerStatus({ ...snapshot(), hardware: { ...snapshot().hardware, board_model: 9 },
      board_configuration: { ...snapshot().settings, reset_cause: 0 } });
    assert.equal(el("#board-model").textContent, "Unknown (9) (RP2350)");
    assert.equal(el("#board-reset-cause").textContent, "Unknown");
  }));

test("partial manufacturer diagnostics remain renderable and diagnostic warnings do not hide behind settings notes",
  () => withDocument(el => {
    for (const manufacturer of [{}, { id: 3 }, { serial_number: 0 }, { product_name: "Card" }]) {
      renderServerStatus({ ...snapshot(), diagnostic_error: "Unavailable diagnostic fields: memory.manufacturer.",
        memory: { ...snapshot().memory, manufacturer } });
      assert.equal(el("#memory-product").textContent, "--");
      assert.equal(el("#memory-serial").textContent, manufacturer.serial_number === 0 ? "00000000" : "--");
      assert.match(el("#status-message").textContent, /Unavailable diagnostic/);
      assert.equal(el("#status-message").hidden, false);
    }
  }));
