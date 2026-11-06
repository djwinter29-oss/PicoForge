// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import { domFixture } from "./dom-fixture.mjs";

let generation = 0;

/** Check fixture observations explicitly; staged SSIDs are candidates, but radio flags remain saved configuration. */
export function assertWirelessConsistent(board, wireless) {
  assert.equal(wireless.wifi_enabled, Boolean(board.settings.radios & 1), "fixture Wi-Fi flag matches boot bits");
  assert.equal(wireless.ble_enabled, Boolean(board.settings.radios & 2), "fixture Bluetooth flag matches boot bits");
}

/** Serve the integration fixture in the shared Python/.NET status shape. */
function serverShape({ msc = null, filesystem_format = null, settings = null, memory = null, board_configuration = null,
  ...status }) {
  return { ...status,
    memory: memory || msc || filesystem_format ? { ...memory, msc, filesystem_format } : null,
    board_configuration: settings || board_configuration ? { ...board_configuration, ...settings } : null };
}

/** A fresh mounted app, board state, and API fault controls for each behavior scenario. */
export async function appFixture(context) {
  const state = {
    statusFails: false, statusUnknownBoard: false, boardScanFails: false, boardWarnings: [],
    ledGate: Promise.resolve(), ledTimeout: false, ledResponseError: false,
    mscGate: Promise.resolve(), mscTimeout: false, mscSucceeds: false,
    saveFails: false, settingsFailure: "save", settingsOutcomeConflict: false, settingsTimeout: false,
    readbackMismatch: false, rebootFails: false, rebootResponseError: false,
    selfTestResponseError: false, selfTestSucceeds: false,
    now: Date.now(),
    responses: new Map(), resyncGate: null, resyncError: null,
    resyncResult: { uncertain: false, control_result: null, wireless: null },
  };
  const fixture = domFixture();
  const { handlers, elements, el: element } = fixture;
  const configureRadio = radio => handlers.get(`#${radio}-configure:click`)();
  const connected = { connected: true, error: null, command_outcome_uncertain: false, reboot_pending: false,
    telemetry_stale: false, storage_stale: false, self_test_stale: false,
    runtime: { led_available: true, led_on: false },
    hardware: { product_name: "PicoForge", board_model: "Pico 2", mcu: "RP2350", capabilities: 2 | 16,
      ram_reserved_bytes: 121008, ram_total_bytes: 532480 },
    settings_supported: true, settings: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0 },
    board_configuration: { monitor_uart_index: 255, monitor_uart_tx_gpio: 255, monitor_uart_rx_gpio: 255,
      storage_gpios: [2, 3, 4, 5, 255, 255], card_detect_gpio: 255, reset_cause: 1 },
    memory: { mode: "spi" }, msc: { enabled: false, transition_pending: false }, self_test: null };
  const secondBoard = structuredClone(connected);
  secondBoard.hardware.board_model = "Pico";
  secondBoard.hardware.mcu = "RP2040";
  secondBoard.settings.system_clock_mhz = 133;
  secondBoard.runtime.led_on = true;
  secondBoard.self_test = { sequence: 91, active: false, state: "idle" };
  const boardStatuses = new Map([["board-a", connected], ["board-b", secondBoard]]);
  const boardWireless = new Map([...boardStatuses.keys()].map(id => [id, {
    wirelessFailure: 0, wirelessError: "Storage busy", wirelessTestGate: null, wirelessTestError: null,
    wirelessState: "connected", wirelessReadFails: false, wirelessStatus: null, wirelessSaved: null,
  }]));
  // Scenario controls target the selected board; requests retain their originating board across awaits.
  for (const field of Object.keys(boardWireless.get("board-a"))) Object.defineProperty(state, field, {
    get: () => boardWireless.get(element("#board-select").value || "board-a")[field],
    set: value => { boardWireless.get(element("#board-select").value || "board-a")[field] = value; },
  });
  const ledRequestedStates = [];
  const requestedUrls = [];
  const savedSettings = [];
  const wirelessSettings = [];
  const testedWireless = [];
  const savedWireless = (board, wireless) => {
    const baseline = { ssid: "saved", wifi_enabled: Boolean(board.settings.radios & 1),
      ble_enabled: Boolean(board.settings.radios & 2), ble_key_configured: Boolean(board.settings.radios & 2),
      tcp_cli_enabled: true, wifi_state: wireless.wirelessState, ipv4: "192.168.1.9" };
    assertWirelessConsistent(board, baseline);
    return { ...baseline, ...wireless.wirelessSaved, ...wireless.wirelessStatus };
  };
  const requestTimeouts = [];
  context.mock.method(globalThis, "setInterval", () => 1);
  context.mock.method(Date, "now", () => state.now);
  context.mock.method(AbortSignal, "timeout", milliseconds => {
    assert.ok([5000, 10000, 15000, 20000, 40000].includes(milliseconds),
      "dashboard requests use endpoint-appropriate deadlines");
    requestTimeouts.push(milliseconds);
    return new AbortController().signal;
  });
  const saved = { document: globalThis.document, window: globalThis.window, fetch: globalThis.fetch };
  globalThis.document = fixture.document;
  globalThis.window = { setInterval: callback => { state.tick = callback; }, confirm: () => true };
  const saveAndConfirm = async () => {
    handlers.get("#settings-save:click")();
    await handlers.get("#confirm-configuration:click")();
  };
  const switchBoard = async id => {
    elements.get("#board-select").value = id;
    handlers.get("#board-select:change")();
    await new Promise(resolve => setImmediate(resolve));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(elements.get("#board-select").value, id, `board ${id} is selectable after the prior request settles`);
  };
  const reconnect = async () => {
    await new Promise(resolve => setImmediate(resolve));
    const connected = boardStatuses.get(elements.get("#board-select").value);
    connected.connected = false;
    await state.tick();
    connected.connected = true;
    connected.reboot_pending = false;
    await state.tick();
    await new Promise(resolve => setImmediate(resolve));
  };
  globalThis.fetch = async (url, options = {}) => {
    requestedUrls.push(url);
    assert.ok(options.signal instanceof AbortSignal, "dashboard API requests have a deadline");
    const boardId = new URL(url, "http://localhost").searchParams.get("board");
    const connected = boardStatuses.get(boardId);
    const radio = boardWireless.get(boardId);
    const response = state.responses.get(url.split("?")[0]);
    if (response) return response(options);
    if (url === "/api/boards") {
      if (state.boardScanFails) throw new Error("scan down");
      return { ok: true, json: async () => ({
        boards: [
          { id: "board-a", product_name: "PicoForge", serial_number: "board-A", device_path: "/hid/a" },
          { id: "board-b", product_name: "PicoForge HID", serial_number: null, device_path: "/hid/b" },
        ],
        default_board_id: "board-a",
        warnings: state.boardWarnings,
      }) };
    }
    if (url.startsWith("/api/control/resync?board=")) {
      assert.deepEqual(JSON.parse(options.body), { confirm: true });
      await state.resyncGate;
      if (state.resyncError) throw state.resyncError;
      if (state.resyncResult.uncertain === false) connected.command_outcome_uncertain = false;
      return { ok: true, json: async () => state.resyncResult };
    }
    if (url.startsWith("/api/status?board=")) {
      if (state.statusFails) throw new Error("status down");
      if (state.statusUnknownBoard) return { ok: false, status: 404,
        json: async () => ({ error: "unknown board; rescan and select a listed board" }) };
      const wireless = connected.hardware.capabilities & 128 ? {
        wireless: radio.wirelessReadFails ? null : savedWireless(connected, radio),
        wireless_error: radio.wirelessReadFails ? "wireless down" : null,
      } : {};
      return { ok: true, json: async () => ({ ...serverShape(structuredClone(connected)), ...wireless }) };
    }
    if (url.startsWith("/api/wireless/test?board=")) {
      assert.ok(options.signal instanceof AbortSignal);
      const request = JSON.parse(options.body);
      testedWireless.push(request);
      await radio.wirelessTestGate;
      if (radio.wirelessTestError) throw radio.wirelessTestError;
      const test_generation = (radio.wirelessStatus?.test_generation ?? 0) % 255 + 1;
      const test_id = test_generation.toString(16).padStart(32, "0");
      radio.wirelessStatus = { ...radio.wirelessStatus, ssid: request.ssid,
        ssid_hex: request.ssid_hex ?? Buffer.from(request.ssid).toString("hex"),
        test_generation, test_id, wifi_state: "test_joined",
        pair_tested: true, ssid_staged: true, password_staged: true };
      return { ok: true, json: async () => ({
        test: { ipv4: "192.168.1.9", remaining_seconds: 600, test_generation, test_id },
      }) };
    }
    if (url.startsWith("/api/wireless/cancel?board=")) {
      radio.wirelessStatus = null;
      return { ok: true, json: async () => ({ cancelled: true }) };
    }
    if (url.startsWith("/api/wireless?board=")) {
      assert.equal(options.method, "POST", "Wi-Fi status is read from /api/status, never GET /api/wireless");
      const settings = JSON.parse(options.body);
      wirelessSettings.push(settings);
      const baseline = savedWireless(connected, radio);
      const wireless = { ...baseline, ...settings, board: undefined, password: undefined, ble_key: undefined,
        ssid_hex: settings.ssid_hex ?? Buffer.from(settings.ssid ?? baseline.ssid).toString("hex"),
        ble_key_configured: settings.ble_key === undefined ? baseline.ble_key_configured : Boolean(settings.ble_key),
        wifi_state: "connected", ssid_staged: false, password_staged: false, pair_tested: false, key_staged: false };
      if (!radio.wirelessFailure) {
        connected.settings = { ...settings.board };
        radio.wirelessSaved = wireless;
        radio.wirelessStatus = null;
        assertWirelessConsistent(connected, wireless);
      }
      return { ok: radio.wirelessFailure === 0, status: radio.wirelessFailure || 200, json: async () => ({
        error: radio.wirelessFailure === 409 ? radio.wirelessError : "Wireless outcome is uncertain",
        uncertain: radio.wirelessFailure === 503 || radio.wirelessError === "Wireless outcome is uncertain",
        reboot_pending: true, settings: settings.board,
        wireless,
      }) };
    }
    if (url.startsWith("/api/settings?board=")) {
      const { confirm_data_loss, ...settings } = JSON.parse(options.body);
      if (confirm_data_loss !== undefined) assert.equal(confirm_data_loss, true);
      assert.equal(Boolean(confirm_data_loss), Boolean(connected.msc.enabled));
      savedSettings.push(settings);
      if (state.settingsTimeout) throw new DOMException("HTTP deadline", "TimeoutError");
      const status = state.settingsOutcomeConflict ? 409
        : state.saveFails ? state.settingsFailure === "unsupported" ? 503 : 409 : 200;
      return { ok: !state.saveFails && !state.settingsOutcomeConflict, status,
        json: async () => ({ error: state.settingsOutcomeConflict
          ? "Forced save outcome is uncertain; the command was not resent"
          : state.settingsFailure === "unsupported" ? "HID settings command is not supported" : "save rejected",
        settings: state.readbackMismatch ? { ...settings, sdcard_profile: 99 } : settings }) };
    }
    if (url.startsWith("/api/reboot?board=")) {
      assert.deepEqual(JSON.parse(options.body), { confirm: true });
      return { ok: !state.rebootFails && !state.rebootResponseError,
        status: state.rebootResponseError ? 409 : state.rebootFails ? 503 : 200,
        json: async () => state.rebootFails || state.rebootResponseError
          ? { error: state.rebootResponseError
            ? "Reboot outcome is uncertain; command was not resent" : "Reboot delivery failed" } : {} };
    }
    if (url.startsWith("/api/led?board=")) {
      await state.ledGate;
      if (state.ledTimeout) throw new DOMException("HTTP deadline", "TimeoutError");
      ledRequestedStates.push(JSON.parse(options.body).on);
      if (state.ledResponseError) return { ok: false, status: 409, json: async () => ({
        error: "LED outcome is uncertain; command was not resent",
      }) };
      return { ok: true, json: async () => ({ control_result: { status: "completed", opcode: 1 } }) };
    }
    if (url.startsWith("/api/msc/force-return?board=")) {
      assert.deepEqual(JSON.parse(options.body), { confirm_data_loss: true });
      await state.mscGate;
      if (state.mscTimeout) throw new DOMException("HTTP deadline", "TimeoutError");
      return { ok: true, json: async () => ({ msc: { enabled: false } }) };
    }
    if (url.startsWith("/api/self-test")) {
      if (state.selfTestResponseError) return { ok: false, status: 409, json: async () => ({
        detail: "Benchmark outcome is uncertain; command was not resent",
      }) };
      if (state.selfTestSucceeds) {
        await state.gate;
        return { ok: true, status: 202, json: async () => ({}) };
      }
    }
    if (url.startsWith("/api/msc?") && state.mscSucceeds) {
      await state.gate;
      if (state.mscTimeout) throw new DOMException("HTTP deadline", "TimeoutError");
      return { ok: true, json: async () => ({ msc: { enabled: false, transition_pending: true } }) };
    }
    await state.gate;
    return { ok: false, status: 503, json: async () => ({ error: "HID command delivery failed" }) };
  };

  context.after(() => Object.assign(globalThis, saved));
  await import(`../../src/js/app.mjs?fixture=${++generation}`);
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  return {
    state, connected, handlers, elements, configureRadio, requestedUrls, savedSettings, wirelessSettings,
    testedWireless, requestTimeouts, ledRequestedStates, saveAndConfirm, switchBoard,
    reconnect, boardStatuses, boardWireless,
  };
}
