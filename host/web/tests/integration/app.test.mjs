// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { renderStatus } from "../../src/js/dashboard.mjs";
import { appFixture } from "../support/app-fixture.mjs";

test("server dashboard presents expected USB reconnection without hiding real errors", () => {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      dataset: {}, valueAsNumber: 32, setAttribute() {}, replaceChildren() {},
    });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({ append() {} }) };
  try {
    renderStatus({ connected: false, reconnecting: true });
    assert.equal(element("#connection-label").textContent, "Reconnecting");
    assert.equal(element("#status-message").textContent, "Reconnecting after USB-drive mode change...");
    assert.equal(element(".status-line").dataset.error, "false");
    assert.equal(element("#settings-note").textContent, "Waiting for a PicoForge connection.");
    assert.equal(element("#status-message").hidden, true);
    renderStatus({ connected: false, reconnecting: true, error: "read error" });
    assert.equal(element("#connection-label").textContent, "Attention");
    assert.equal(element("#status-message").textContent, "read error");
    assert.equal(element(".status-line").dataset.error, "true");
    assert.equal(element("#status-message").hidden, false);
    const shared = { connected: true, settings_supported: true, storage_stale: false, hardware: { capabilities: 128 | 2 },
      wireless: { ssid: "saved", wifi_enabled: true, ble_enabled: false,
        ble_key_configured: false, tcp_cli_enabled: true },
      settings: { system_clock_mhz: 200, sdcard_profile: 0, radios: 1 },
      wireless_dirty: true, wireless_stage_valid: true, msc: { enabled: true, transition_pending: false } };
    renderStatus(shared);
    assert.equal(element("#settings-save").disabled, true);
    assert.match(element("#settings-note").textContent, /safely eject.*Return to Pico/);
    assert.equal(element("#settings-save").title, element("#settings-note").textContent);
    renderStatus({ ...shared, msc: { enabled: false, transition_pending: false } });
    assert.equal(element("#settings-save").disabled, false);
    assert.equal(element("#settings-save").title, "");
  } finally {
    globalThis.document = saved;
  }
});

test("startup polling and drafts recover after status and scan failures", async context => {
  const { state, handlers, elements, requestedUrls } = await appFixture(context);
  assert.equal(typeof handlers.get("#reboot-control:click"), "function");
  assert.equal(typeof state.tick, "function", "startup must register status polling");
  assert.equal(elements.get("#connection-label").textContent, "Connected",
    elements.get("#status-message").textContent);
  assert.equal(elements.get("#settings-note").textContent, "");
  assert.equal(elements.get("#settings-note").hidden, true);
  const statusRequests = () => requestedUrls.filter(url => url.startsWith("/api/status?")).length;
  const visibleStatusRequests = statusRequests();
  globalThis.document.hidden = true;
  state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(statusRequests(), visibleStatusRequests, "background tabs pause status polling");
  globalThis.document.hidden = false;
  handlers.get("document:visibilitychange")();
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(statusRequests(), visibleStatusRequests + 1, "returning to the tab refreshes status immediately");
  elements.get("#settings-clock").value = "250";
  handlers.get("#settings-clock:change")();
  state.statusFails = true;
  await state.tick();
  assert.equal(elements.get("#settings-clock").value, "250", "a transient status error preserves settings drafts");
  assert.equal(elements.get("#settings-clock").disabled, true, "commands stay disabled while status is stale");
  assert.match(elements.get("#status-message").textContent, /Status unavailable/);
  state.statusFails = false;
  await state.tick();
  assert.equal(elements.get("#settings-clock").value, "250");
  const scansBefore = requestedUrls.filter(url => url === "/api/boards").length;
  state.statusUnknownBoard = true;
  await state.tick();
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(requestedUrls.filter(url => url === "/api/boards").length, scansBefore + 1,
    "a restarted server's unknown-board reply triggers one rate-limited rescan");
  state.statusUnknownBoard = false;
  await state.tick();
  assert.equal(elements.get("#board-select").value, "board-a", "the rescan keeps the selected board");
  elements.get("#settings-clock").value = "200";
  handlers.get("#settings-clock:change")();
  state.boardScanFails = true;
  await handlers.get("#rescan-boards:click")();
  assert.equal(elements.get("#settings-clock").value, "200",
    "a failed rescan preserves the selected board and drafts");
  assert.match(elements.get("#status-message").textContent, /Board scan failed/);
  state.boardScanFails = false;
  await state.tick();
});

test("board identity warnings stay visible across status polls and clear on a clean rescan", async context => {
  const { state, handlers, elements } = await appFixture(context);
  state.boardWarnings = ["Duplicate PicoForge board identity; disconnect boards with duplicate serials"];
  await handlers.get("#rescan-boards:click")();
  assert.match(elements.get("#status-message").textContent, /Duplicate PicoForge/);
  await state.tick();
  assert.match(elements.get("#status-message").textContent, /Duplicate PicoForge/);
  state.boardWarnings = [];
  await handlers.get("#rescan-boards:click")();
  assert.doesNotMatch(elements.get("#status-message").textContent, /Duplicate PicoForge/);
});

test("settings presentation respects active and pending self-tests", async context => {
  const { state, connected, elements } = await appFixture(context);
  for (const testState of [
    { self_test: { sequence: 1, active: true, state: "writing" } },
    { self_test_command_pending: true },
  ]) {
    renderStatus({ ...connected, ...testState });
    assert.match(elements.get("#settings-note").textContent, /benchmark/);
    assert.equal(elements.get("#settings-note").hidden, false);
    for (const selector of ["#settings-clock", "#settings-storage", "#settings-save", "#reboot-control"]) {
      assert.equal(elements.get(selector).disabled, true);
    }
    renderStatus({ ...connected, ...testState, settings_dirty: true });
    assert.equal(elements.get("#settings-save").disabled, true);
    assert.match(elements.get("#settings-note").textContent, /benchmark/);
    assert.equal(elements.get("#settings-note").hidden, false);
    renderStatus({ ...connected, ...testState, settings_message: "Save failed: device busy" });
    assert.equal(elements.get("#settings-note").textContent, "Save failed: device busy");
    assert.equal(elements.get("#settings-note").hidden, false);
  }
  renderStatus(connected);
  assert.equal(elements.get("#product-name").textContent, "PicoForge (SDIO)");
  assert.equal(elements.get("#settings-clock").disabled, false);
  assert.equal(elements.get("#settings-storage").disabled, false);
  assert.equal(Boolean(elements.get("#reboot-control").disabled), false);
  renderStatus({ ...connected, settings_dirty: true });
  assert.equal(elements.get("#settings-save").disabled, false);
  assert.equal(elements.get("#settings-note").textContent, "");
  assert.equal(elements.get("#settings-note").hidden, true);
});

test("self-test delivery and sequence changes lock commands without replay", async context => {
  const { state, connected, handlers, elements, requestedUrls } = await appFixture(context);
  state.selfTestSucceeds = true;
  connected.filesystem_format = "exfat";
  connected.memory = { initialized: true };
  connected.self_test = { sequence: 254, active: false, state: "idle" };
  await state.tick();
  const gatedControls = ["#self-test-start", "#self-test-size", "#settings-clock",
    "#settings-storage", "#settings-save", "#reboot-control", "#msc-control"];
  assert.equal(elements.get("#self-test-start").disabled, false);
  state.gate = new Promise(resolve => { state.release = resolve; });
  const starting = handlers.get("#self-test-start:click")();
  for (const sequence of [255, 0, 1]) {
    connected.self_test.sequence = sequence;
    await state.tick();
    for (const selector of gatedControls) {
      assert.equal(elements.get(selector).disabled, true, `${selector} must stay disabled during Start delivery`);
    }
  }
  const startsSent = requestedUrls.filter(url => url.startsWith("/api/self-test")).length;
  await handlers.get("#self-test-start:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/self-test")).length, startsSent);
  state.release();
  await starting;
  await state.tick();
  assert.equal(elements.get("#self-test-start").disabled, true, "wait for a newer report after delivery");
  connected.self_test = { sequence: 2, active: true, state: "writing" };
  await state.tick();
  for (const selector of gatedControls) {
    assert.equal(elements.get(selector).disabled, true, `${selector} must stay disabled during execution`);
  }
  assert.equal(elements.get("#self-test-cancel").disabled, false);
  connected.self_test = { sequence: 3, active: false, state: "passed" };
  await state.tick();
  assert.equal(elements.get("#self-test-start").disabled, false);
  assert.equal(elements.get("#settings-clock").disabled, false);
  assert.equal(elements.get("#reboot-control").disabled, false);
});

test("USB sharing confirmation and ownership transitions fence commands", async context => {
  const { state, connected, handlers, elements, requestedUrls } = await appFixture(context);
    connected.self_test = { sequence: 3, active: false, state: "passed" };
    connected.memory = { initialized: true };
    connected.filesystem_format = "exfat";
    await state.tick();
  state.mscSucceeds = true;
  for (const rejected of [true, false]) {
    connected.msc = { enabled: false, transition_pending: false };
    await state.tick();
    const benchmark = elements.get('[aria-labelledby="self-test-title"]');
    assert.equal(Boolean(benchmark.hidden), false);
    state.gate = new Promise(resolve => { state.release = resolve; });
    const beforeShare = requestedUrls.length;
    await handlers.get("#msc-control:click")();
    assert.equal(elements.get("#share-dialog").open, true);
    assert.equal(requestedUrls.length, beforeShare);
    assert.equal(Boolean(benchmark.hidden), false);
    handlers.get("#cancel-share:click")();
    await handlers.get("#confirm-share:click")();
    assert.equal(requestedUrls.length, beforeShare);
    await handlers.get("#msc-control:click")();
    handlers.get("#share-dialog:keydown")({ key: "Escape", preventDefault() {} });
    await handlers.get("#confirm-share:click")();
    assert.equal(requestedUrls.length, beforeShare);
    await handlers.get("#msc-control:click")();
    connected.self_test.active = true;
    await state.tick();
    await handlers.get("#confirm-share:click")();
    assert.equal(requestedUrls.length, beforeShare + 1, "only status polling, no USB command");
    connected.self_test.active = false;
    await state.tick();
    await handlers.get("#msc-control:click")();
    const sharing = handlers.get("#confirm-share:click")();
    assert.equal(benchmark.hidden, true, "hide benchmark immediately on USB enable");
    state.release();
    await sharing;
    for (const snapshot of [
      { connected: false, msc: null },
      { connected: true, msc: { enabled: false, transition_pending: false } },
      { connected: true, msc: { enabled: true, transition_pending: true } },
    ]) {
      Object.assign(connected, snapshot);
      await state.tick();
      assert.equal(benchmark.hidden, true, "intermediate status must not restore benchmark");
      assert.equal(elements.get("#msc-control").disabled, true);
      assert.equal(elements.get("#self-test-start").disabled, true);
    }
    connected.msc = { enabled: !rejected, transition_pending: false, request_rejected: rejected };
    await state.tick();
    assert.equal(benchmark.hidden, !rejected);
    if (!rejected) {
      await handlers.get("#msc-control:click")();
      await handlers.get("#confirm-return:click")();
      assert.equal(benchmark.hidden, true, "return must wait for ownership telemetry");
      connected.msc = { enabled: false, transition_pending: false };
      await new Promise(resolve => setImmediate(resolve));
      await state.tick();
      assert.equal(Boolean(benchmark.hidden), false);
    }
  }
});

test("storage telemetry, pin rows, and dashboard layout preserve presentation", async context => {
  const { state, connected, elements } = await appFixture(context);
  state.mscSucceeds = false;
  state.selfTestSucceeds = false;
  connected.memory = { mode: "spi" };
  connected.self_test = null;
  delete connected.filesystem_format;
  renderStatus({ ...connected, memory: { configured: true, sample_valid: false, initialized: false } });
  assert.equal(elements.get("#memory-state").textContent, "Busy");
  assert.equal(elements.get("#memory-note").textContent, "");
  assert.equal(elements.get("#memory-speed").textContent, "--");
  for (const speedMode of ["high-speed", 2]) {
    renderStatus({ ...connected, memory: { configured: true, sample_valid: true, initialized: true,
      present: true, bus_clock_hz: 50000000, speed_mode: speedMode } });
    assert.equal(elements.get("#memory-state").textContent, "Ready");
    assert.equal(elements.get("#memory-speed").textContent, "50 MHz");
  }
  const validStorage = { ...connected, filesystem_format: "exfat", memory: {
    configured: true, sample_valid: true, initialized: true, present: true, mode: "sdio",
    bus_clock_hz: 50000000, speed_mode: "high-speed", capacity_bytes: 127865454592,
    manufacturer: { id: 3, product_name: "SB128", product_revision: "8.0", manufacture_year: 2020,
      manufacture_month: 1, serial_number: 4089330880 },
  } };
  const busyStorage = { ...connected, memory: { configured: true, sample_valid: false } };
  renderStatus(validStorage);
  const detailIds = ["#memory-speed", "#memory-size", "#memory-filesystem",
    "#memory-manufacturer", "#memory-product", "#memory-serial"];
  const details = detailIds.map(id => elements.get(id).textContent);
  for (let sample = 0; sample < 3; sample++) {
    renderStatus(busyStorage);
    assert.equal(elements.get("#memory-state").textContent, "Busy");
    assert.equal(elements.get("#memory-note").textContent, "");
    assert.deepEqual(detailIds.map(id => elements.get(id).textContent), details);
  }
  for (const invalidation of [
    { connected: false },
    { ...connected, storage_stale: true },
    { ...connected, memory: { configured: true, sample_valid: true, presence_known: true, present: false } },
    { ...connected, memory: { configured: true, sample_valid: true, present: true, initialized: false } },
    { ...validStorage, msc: { enabled: true } },
    { ...validStorage, msc: { transition_pending: true } },
  ]) {
    renderStatus(validStorage);
    renderStatus(invalidation);
    renderStatus(busyStorage);
    assert.equal(elements.get("#memory-speed").textContent, "--", "invalidated details must not return");
  }
  renderStatus(connected);
  for (const transport of ["sdio", "spi"]) {
    connected.hardware.capabilities = 2 | (transport === "spi" ? 8 : 16);
    renderStatus({ ...connected, board_configuration: {
      storage_transport: transport, storage_gpios: [2, 3, 4, 5, 6, 7], card_detect_gpio: 8,
      monitor_uart_index: 0, monitor_uart_tx_gpio: 0, monitor_uart_rx_gpio: 255, reset_cause: 2,
    } });
    assert.equal(elements.get("#product-name").textContent,
      `PicoForge (${transport === "spi" ? "SPI" : "SDIO"})`);
    const rows = id => elements.get(id).children.map(row => row.children.map(cell => cell.textContent));
    assert.deepEqual(rows("#board-uart"), [["TX", "RX"], ["GP0", "Unassigned"]]);
    assert.deepEqual(rows("#board-storage-pins"), transport === "spi"
      ? [["CLK", "MOSI", "MISO", "CS", "Card detect"], ["GP2", "GP3", "GP4", "GP5", "GP8"]]
      : [["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3", "Card detect"],
        ["GP2", "GP3", "GP4", "GP5", "GP6", "GP7", "GP8"]]);
    const uartRows = elements.get("#board-uart").children;
    const storageRows = elements.get("#board-storage-pins").children;
    renderStatus({ ...connected, board_configuration: {
      storage_transport: transport, storage_gpios: [2, 3, 4, 5, 6, 7], card_detect_gpio: 8,
      monitor_uart_index: 0, monitor_uart_tx_gpio: 0, monitor_uart_rx_gpio: 255, reset_cause: 2,
    } });
    assert.strictEqual(elements.get("#board-uart").children, uartRows,
      "unchanged UART pin configuration keeps its rendered rows");
    assert.strictEqual(elements.get("#board-storage-pins").children, storageRows,
      "unchanged storage pin configuration keeps its rendered rows");
  }
  renderStatus(connected);
  assert.equal(elements.get("#board-model").textContent, "Pico 2 (RP2350)");
  assert.equal(elements.get("#ram-usage").textContent, "118.2 / 520 KiB (22.7%)");
  const html = readFileSync(new URL("../../src/index.html", import.meta.url), "utf8");
  const identity = html.split('aria-label="Hardware identity"')[1].split("</section>")[0];
  assert.deepEqual([...identity.matchAll(/<strong id="([^"]+)"/g)].map(match => match[1]),
    ["product-name", "board-model", "serial", "identity-clock", "ram-usage", "firmware-version"]);
  assert.doesNotMatch(html, /id="ram-reserved"|id="ram-total"/);
  assert.doesNotMatch(html, /id="mcu"|Microcontroller/);
  assert.match(html, />Save&amp;Reboot<\/button>/);
  assert.match(html, />Reboot Board<\/button>/);
  assert.match(html, /id="settings-title">Boot configuration<\/h2>/);
  assert.doesNotMatch(html, /Board configuration/);
  assert.doesNotMatch(html, /BOOT CONFIGURATION/);
  assert.doesNotMatch(html, /class="eyebrow"/);
  assert.doesNotMatch(html, /metric-product/);
  const diagnostics = html.split('<dl class="runtime-grid runtime-summary">')[1].split("</dl>")[0];
  assert.deepEqual([...diagnostics.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Uptime", "Reset cause", "Service load (Core 0 / Core 1)", "Board temperature", "CLI port", "Storage bus speed",
      "Wi-Fi", "Bluetooth"]);
  const storage = html.split('aria-labelledby="memory-title"')[1].split("</section>")[0];
  assert.deepEqual([...storage.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Memory state", "Capacity", "Filesystem", "Manufacturer", "Product", "Serial"]);
  assert.doesNotMatch(html, /id="memory-mode"/);
  const pins = html.match(/<details class="pin-map-disclosure"([^>]*)>(\s*<summary>Pin configuration[\s\S]*?)<\/details>/);
  assert.ok(pins);
  assert.doesNotMatch(pins[1], /\bopen\b/);
  assert.match(pins[2], /<summary>Pin configuration<\/summary>/);
  assert.doesNotMatch(pins[2], /board-reset-cause/);
  assert.match(diagnostics, /<dt>Reset cause<\/dt><dd id="board-reset-cause">/);
  for (const id of ["board-uart", "board-storage-pins"]) {
    assert.ok(pins[2].includes(`id="${id}"`));
  }
});

test("settings, reboot and LED recovery preserve per-board command ownership", async context => {
  const {
    state, connected, handlers, elements, requestedUrls, savedSettings, ledRequestedStates, saveAndConfirm, switchBoard,
    reconnect,
  } = await appFixture(context);
  assert.equal(elements.get("#board-select").value, "board-a");
  assert.equal(elements.get("#board-select").children.find(option => option.value === "board-a").textContent,
    "PicoForge / board-A");
  assert.equal(elements.get("#board-select").children.find(option => option.value === "board-b").textContent,
    "PicoForge / HID /hid/b");
  assert.ok(requestedUrls.includes("/api/status?board=board-a"));
  elements.get("#settings-clock").value = "250";
  handlers.get("#settings-clock:change")();
  const beforeCancel = requestedUrls.length;
  await handlers.get("#settings-save:click")();
  assert.equal(elements.get("#configuration-dialog").open, true);
  assert.match(elements.get("#configuration-dialog-profile").textContent, /250 MHz.*Standard/);
  handlers.get("#cancel-configuration:click")();
  assert.equal(elements.get("#configuration-dialog").open, false);
  await handlers.get("#reboot-control:click")();
  assert.equal(elements.get("#reboot-dialog").open, true);
  assert.match(elements.get("#reboot-consequence").textContent, /Unsaved.*lost/);
  handlers.get("#cancel-reboot:click")();
  handlers.get("#reboot-control:click")();
  handlers.get("#reboot-dialog:keydown")({ key: "Escape", preventDefault() {} });
  assert.equal(elements.get("#reboot-dialog").open, false);
  await handlers.get("#confirm-reboot:click")();
  await handlers.get("#confirm-configuration:click")();
  assert.equal(requestedUrls.length, beforeCancel);
  await saveAndConfirm();
  assert.deepEqual(savedSettings[0], { system_clock_mhz: 250, sdcard_profile: 0, radios: 0 });
  assert.equal(requestedUrls.at(-1), "/api/settings?board=board-a");
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/reboot")).length, 0);
  connected.settings = { ...savedSettings[0] };
  await reconnect();
  elements.get("#settings-storage").value = "1";
  handlers.get("#settings-storage:change")();
  await saveAndConfirm();
  assert.deepEqual(savedSettings[1], { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 });
  connected.settings = { ...savedSettings[1] };
  await reconnect();
  elements.get("#settings-clock").value = "200";
  handlers.get("#settings-clock:change")();
  for (const failure of ["save", "readback", "unsupported"]) {
    state.saveFails = failure !== "readback";
    state.settingsFailure = failure;
    state.readbackMismatch = failure === "readback";
    await saveAndConfirm();
    assert.equal(savedSettings.length, 3 + ["save", "readback", "unsupported"].indexOf(failure));
    assert.equal(requestedUrls.filter(url => url.startsWith("/api/reboot")).length, 0,
      "failed save must not send a separate reboot");
    assert.match(elements.get("#settings-note").textContent, failure === "readback" ? /outcome is uncertain/ : /Save failed/);
    assert.equal(elements.get("#settings-note").hidden, false);
    if (failure === "readback") {
      connected.settings = { ...savedSettings.at(-1) };
      await reconnect();
      elements.get("#settings-clock").value = "250";
      handlers.get("#settings-clock:change")();
    }
  }
  state.saveFails = false;
  state.readbackMismatch = false;
  await saveAndConfirm();
  assert.match(elements.get("#settings-note").textContent, /Settings saved. Reboot requested/);
  connected.settings = { ...savedSettings.at(-1) };
  await reconnect();
  state.settingsOutcomeConflict = true;
  elements.get("#settings-clock").value = "200";
  handlers.get("#settings-clock:change")();
  const beforeUncertainConflict = savedSettings.length;
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeUncertainConflict + 1);
  assert.match(elements.get("#settings-note").textContent, /outcome is uncertain/);
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeUncertainConflict + 1,
    "an explicit uncertain HTTP 409 is not treated as a definite rejection");
  state.settingsOutcomeConflict = false;
  connected.settings = { ...connected.settings, ...savedSettings.at(-1) };
  await state.tick();
  assert.match(elements.get("#settings-note").textContent, /readback matches/);
  connected.settings = { ...connected.settings, system_clock_mhz: 200, sdcard_profile: 0 };
  await state.tick();
  state.settingsTimeout = true;
  elements.get("#settings-clock").value = "250";
  handlers.get("#settings-clock:change")();
  const beforeUncertainSave = savedSettings.length;
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeUncertainSave + 1);
  assert.match(elements.get("#settings-note").textContent, /outcome is uncertain/);
  assert.equal(elements.get("#settings-save").disabled, true);
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeUncertainSave + 1, "an uncertain save is not resent");
  state.settingsTimeout = false;
  const oldSettings = { ...connected.settings };
  await switchBoard("board-b");
  await switchBoard("board-a");
  assert.equal(elements.get("#settings-save").disabled, true,
    "switching back restores the unresolved settings outcome");
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeUncertainSave + 1,
    "the unresolved settings save is not replayed after switching boards");
  connected.settings = oldSettings;
  connected.connected = false;
  await state.tick();
  connected.connected = true;
  await state.tick();
  assert.equal(elements.get("#settings-clock").disabled, true,
    "disconnect alone does not clear an uncertain settings outcome");
  connected.settings = { ...connected.settings, ...savedSettings.at(-1) };
  for (let attempt = 0; attempt < 3; attempt++) {
    await state.tick();
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.match(elements.get("#settings-note").textContent, /readback matches/);
  assert.equal(elements.get("#settings-save").disabled, true, "matching status resolves the pending save");
  await switchBoard("board-b");
  await switchBoard("board-a");
  assert.equal(elements.get("#settings-clock").disabled, false,
    "reconciliation remains cleared after switching away and back again");
  connected.settings = { ...connected.settings, system_clock_mhz: 200, sdcard_profile: 0 };
  await state.tick();
  state.rebootFails = true;
  await handlers.get("#reboot-control:click")();
  await handlers.get("#confirm-reboot:click")();
  assert.match(elements.get("#settings-note").textContent, /outcome is uncertain/);
  state.rebootFails = false;
  const rebootRequests = requestedUrls.filter(url => url.startsWith("/api/reboot?")).length;
  await handlers.get("#reboot-control:click")();
  await handlers.get("#confirm-reboot:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/reboot?")).length, rebootRequests,
    "a server error with an unknown reboot outcome is not resent");
  await reconnect();
  await handlers.get("#reboot-control:click")();
  await handlers.get("#confirm-reboot:click")();
  assert.equal(requestedUrls.at(-1), "/api/reboot?board=board-a");
  await reconnect();
  state.ledResponseError = true;
  const ledBeforeConflict = requestedUrls.filter(url => url.startsWith("/api/led?")).length;
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, ledBeforeConflict + 1);
  assert.match(elements.get("#status-message").textContent, /LED outcome is uncertain; command was not resent/);
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, ledBeforeConflict + 1,
    "an uncertain LED 409 is not resent");
  state.ledResponseError = false;
  connected.runtime.led_on = ledRequestedStates.at(-1);
  await new Promise(resolve => setImmediate(resolve));
  await state.tick();
  assert.doesNotMatch(elements.get("#status-message").textContent, /LED outcome is uncertain/,
    `reconciled LED state: ${connected.runtime.led_on}`);
  connected.runtime.led_on = false;
  await state.tick();
  state.rebootResponseError = true;
  const rebootBeforeConflict = requestedUrls.filter(url => url.startsWith("/api/reboot?")).length;
  await handlers.get("#reboot-control:click")();
  await handlers.get("#confirm-reboot:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/reboot?")).length, rebootBeforeConflict + 1,
    `${elements.get("#reboot-control").disabled}; ${elements.get("#status-message").textContent}; `
    + elements.get("#settings-note").textContent);
  assert.match(elements.get("#settings-note").textContent, /Reboot outcome is uncertain; command was not resent/);
  state.rebootResponseError = false;
  await reconnect();
  connected.self_test = { sequence: 5, active: false, state: "idle" };
  connected.memory = { initialized: true };
  connected.filesystem_format = "exfat";
  await new Promise(resolve => setImmediate(resolve));
  await state.tick();
  state.selfTestResponseError = true;
  const selfTestBeforeConflict = requestedUrls.filter(url => url.startsWith("/api/self-test")).length;
  await handlers.get("#self-test-start:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/self-test")).length, selfTestBeforeConflict + 1);
  assert.match(elements.get("#status-message").textContent, /Benchmark outcome is uncertain; command was not resent/);
  await handlers.get("#self-test-start:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/self-test")).length, selfTestBeforeConflict + 1,
    "an uncertain benchmark 409 is not resent");
  state.selfTestResponseError = false;
  await switchBoard("board-b");
  await switchBoard("board-a");
  const selfTestRequestsAfterSwitch = requestedUrls.filter(url => url.startsWith("/api/self-test")).length;
  await handlers.get("#self-test-start:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/self-test")).length, selfTestRequestsAfterSwitch,
    "switching back restores the benchmark sequence awaiting reconciliation");
  connected.self_test = { sequence: 7, active: false, state: "idle" };
  await state.tick();
  assert.equal(elements.get("#self-test-start").disabled, true, "periodic reports cannot confirm unknown delivery");
  connected.command_resync_supported = true;
  state.resyncResult = { uncertain: false, control_result: { opcode: 4, status: "failed" } };
  await handlers.get("#command-resync:click")();
  for (const name of ["#led-control:click", "#msc-control:click", "#self-test-start:click"]) {
    state.gate = new Promise(resolve => { state.release = resolve; });
    state.statusFails = false;
    state.tick();
    await new Promise(resolve => setImmediate(resolve));
    let pending = handlers.get(name)?.();
    if (name === "#msc-control:click") {
      await pending;
      pending = handlers.get("#confirm-share:click")();
    }
    state.statusFails = true;
    state.tick();
    await new Promise(resolve => setImmediate(resolve));
    state.release();
    await assert.doesNotReject(async () => pending, name);
  }
  assert.ok(requestedUrls.includes("/api/led?board=board-a"));
  connected.self_test = { sequence: 9, active: false, state: "idle" };
  connected.msc.request_rejected = true;
  state.statusFails = false;
  await state.tick();
  connected.msc.request_rejected = false;
  await handlers.get("#command-resync:click")();
  await new Promise(resolve => setImmediate(resolve));
  const ledRequestsBeforeGate = requestedUrls.filter(url => url.startsWith("/api/led?")).length;
  let finishLed;
  state.ledGate = new Promise(resolve => { finishLed = resolve; });
  const pendingLed = handlers.get("#led-control:click")();
  await new Promise(resolve => setImmediate(resolve));
  const ledRequests = requestedUrls.filter(url => url.startsWith("/api/led?")).length;
  assert.equal(ledRequests, ledRequestsBeforeGate + 1, "the LED request remains in flight while its response is gated");
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, ledRequests,
    "a second LED action is ignored while the first is pending");
  elements.get("#board-select").value = "board-b";
  handlers.get("#board-select:change")();
  assert.equal(elements.get("#board-select").value, "board-a",
    "board selection stays locked while a command request is in flight");
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  finishLed();
  await pendingLed;
  state.ledGate = Promise.resolve();
  assert.equal(elements.get("#led-control").textContent, "Board LED: On",
    "the in-flight LED response updates its still-selected board");
  elements.get("#board-select").value = "board-a";
  handlers.get("#board-select:change")();
  await new Promise(resolve => setImmediate(resolve));
  state.statusFails = false;
  connected.runtime.led_on = false;
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  state.ledTimeout = true;
  const beforeUncertainLed = requestedUrls.filter(url => url.startsWith("/api/led?")).length;
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, beforeUncertainLed + 1);
  assert.match(elements.get("#status-message").textContent, /LED command outcome is uncertain/);
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, beforeUncertainLed + 1,
    "an uncertain LED command is not resent");
  state.ledTimeout = false;
  await switchBoard("board-b");
  await switchBoard("board-a");
  const ledRequestsAfterSwitch = requestedUrls.filter(url => url.startsWith("/api/led?")).length;
  await handlers.get("#led-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/led?")).length, ledRequestsAfterSwitch,
    "switching back restores the unresolved LED outcome and prevents replay");
  connected.runtime.led_on = true;
  await new Promise(resolve => setImmediate(resolve));
  await state.tick();
  assert.equal(elements.get("#led-control").textContent, "Board LED: On",
    "fresh LED status reconciles the timed-out command");
  connected.msc = { enabled: true, transition_pending: false };
  await state.tick();
  assert.equal(elements.get("#settings-clock").disabled, false, "local editing is safe during host ownership");
  elements.get("#settings-clock").value = "250";
  handlers.get("#settings-clock:change")();
  await handlers.get("#settings-save:click")();
  assert.equal(elements.get("#configuration-force-warning").hidden, false);
  const beforeForcedSave = savedSettings.length;
  handlers.get("#cancel-configuration:click")();
  await handlers.get("#confirm-configuration:click")();
  assert.equal(savedSettings.length, beforeForcedSave);
  await saveAndConfirm();
  assert.equal(savedSettings.length, beforeForcedSave + 1);
  assert.equal(elements.get("#reboot-control").disabled, true, "standalone reboot stays safe");
  connected.settings = { ...savedSettings.at(-1) };
  await reconnect();
  const beforeReturn = requestedUrls.length;
  await handlers.get("#msc-control:click")();
  assert.equal(elements.get("#return-dialog").open, true);
  assert.equal(requestedUrls.length, beforeReturn);
  handlers.get("#cancel-return:click")();
  await handlers.get("#confirm-return:click")();
  assert.equal(requestedUrls.length, beforeReturn);
  await handlers.get("#msc-control:click")();
  handlers.get("#return-dialog:keydown")({ key: "Escape", preventDefault() {} });
  assert.equal(elements.get("#return-dialog").open, false);
  assert.equal(requestedUrls.length, beforeReturn);
  await handlers.get("#msc-control:click")();
  let finishMsc;
  state.mscGate = new Promise(resolve => { finishMsc = resolve; });
  const pendingMsc = handlers.get("#confirm-return:click")();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(requestedUrls.at(-1), "/api/msc/force-return?board=board-a");
  elements.get("#board-select").value = "board-b";
  handlers.get("#board-select:change")();
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  finishMsc();
  await pendingMsc;
  state.mscGate = Promise.resolve();
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#msc-control").textContent, "Return to Pico",
    "a late storage response must not overwrite the newly selected board");
  assert.equal(elements.get("#return-dialog").open, false);
  elements.get("#board-select").value = "board-b";
  handlers.get("#board-select:change")();
  await new Promise(resolve => setImmediate(resolve));
  assert.ok(requestedUrls.includes("/api/status?board=board-b"));
  assert.equal(elements.get("#wifi-settings").hidden, true);
});

test("wireless provisioning and cancellation preserve draft and uncertainty invariants", async context => {
  const {
    state, boardStatuses, handlers, elements, configureRadio, requestedUrls, savedSettings, wirelessSettings,
    testedWireless, requestTimeouts, saveAndConfirm, switchBoard, reconnect,
  } = await appFixture(context);
  const connected = boardStatuses.get("board-b");
  await switchBoard("board-b");
  connected.hardware.capabilities = 128 | 16 | 2;
  connected.settings.radios = 3;
  state.wirelessStatus = { ble_enabled: true, ble_key_configured: true };
  connected.storage_stale = false;
  connected.msc = { enabled: false, transition_pending: false };
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-settings").hidden, false);
  assert.equal(elements.get("#ble-settings").hidden, false);
  assert.equal(elements.get("#settings-wifi").value, "1");
  assert.equal(elements.get("#settings-ble").value, "1");
  assert.equal(elements.get("#wifi-ssid").value, "saved");
  assert.equal(elements.get("#wifi-state").textContent, "saved (192.168.1.9)");
  state.mscSucceeds = true;
  state.mscTimeout = true;
  state.gate = Promise.resolve();
  await handlers.get("#msc-control:click")();
  const mscRequests = requestedUrls.filter(url => url.startsWith("/api/msc?")).length;
  await handlers.get("#confirm-share:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/msc?")).length, mscRequests + 1);
  assert.match(elements.get("#status-message").textContent, /outcome is uncertain/);
  assert.equal(elements.get("#msc-status-refresh").hidden, false);
  await handlers.get("#confirm-share:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/msc?")).length, mscRequests + 1,
    "an uncertain MSC command is not resent while ownership is unresolved");
  const statusReadsBeforeRefresh = requestedUrls.filter(url => url.startsWith("/api/status?")).length;
  for (let attempt = 0; elements.get("#msc-status-refresh").disabled && attempt < 10; attempt++) {
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.equal(elements.get("#msc-status-refresh").disabled, false);
  await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  await handlers.get("#msc-status-refresh:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/status?")).length, statusReadsBeforeRefresh + 1,
    "the user can explicitly refresh ownership while a command outcome is uncertain");
  await new Promise(resolve => setImmediate(resolve));
  state.mscTimeout = false;
  await switchBoard("board-a");
  await switchBoard("board-b");
  const mscRequestsAfterSwitch = requestedUrls.filter(url => url.startsWith("/api/msc?")).length;
  await handlers.get("#msc-control:click")();
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/msc?")).length, mscRequestsAfterSwitch,
    "switching back restores unresolved storage ownership and prevents command replay");
  connected.msc = { enabled: true, transition_pending: false };
  await state.tick();
  assert.equal(elements.get("#msc-control").textContent, "Return to Pico",
    "fresh ownership status reconciles the timed-out MSC command");
  assert.equal(elements.get("#msc-status-refresh").hidden, true);
  connected.msc = { enabled: false, transition_pending: false };
  await state.tick();
  await switchBoard("board-b");
  const beforeWirelessReads = requestedUrls.filter(url => url.startsWith("/api/wireless?")).length;
  state.wirelessState = "connecting";
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-state").textContent, "saved (connecting)");
  state.wirelessState = "failed";
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-state").textContent, "saved (disconnected)");
  state.wirelessReadFails = true;
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-state").textContent, "Unavailable");
  assert.match(elements.get("#wifi-state").title, /wireless down/);
  assert.equal(elements.get("#connection-label").textContent, "Connected");
  state.wirelessReadFails = false;
  state.wirelessState = "connected";
  await state.tick();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-state").textContent, "saved (192.168.1.9)");
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless?")).length, beforeWirelessReads,
    "Wi-Fi status arrives with /api/status; no separate wireless polling");
  assert.equal(elements.get("#settings-save").disabled, true);
  configureRadio("wifi");
  handlers.get("#wifi-other:click")();
  elements.get("#wifi-ssid").value = "new-network";
  handlers.get("#wifi-ssid:input")();
  elements.get("#wifi-password").value = "new-passphrase";
  handlers.get("#wifi-password:input")();
  assert.equal(elements.get("#settings-save").disabled, true, "unverified Wi-Fi credentials cannot be saved");
  let finishWirelessTest;
  state.wirelessTestGate = new Promise(resolve => { finishWirelessTest = resolve; });
  const testing = handlers.get("#wifi-connect:click")();
  assert.equal(elements.get("#board-select").disabled, true, "board selection locks immediately during a test");
  assert.equal(elements.get("#rescan-boards").disabled, true, "board rescan locks immediately during a test");
  await state.tick();
  assert.equal(elements.get("#board-select").disabled, true);
  assert.equal(elements.get("#rescan-boards").disabled, true);
  assert.equal(elements.get("#led-control").disabled, true);
  assert.equal(elements.get("#msc-control").disabled, true);
  assert.equal(elements.get("#wifi-connect").textContent, "Testing…");
  const beforeSwitch = requestedUrls.length;
  elements.get("#board-select").value = "board-a";
  handlers.get("#board-select:change")();
  assert.equal(elements.get("#board-select").value, "board-b", "a rejected selection restores the actual board");
  assert.equal(requestedUrls.length, beforeSwitch, "board switching must not interrupt a pending test");
  finishWirelessTest();
  await testing;
  for (const timeout of [5000, 10000, 15000, 40000]) {
    assert.ok(requestTimeouts.includes(timeout), `expected a ${timeout} ms request deadline`);
  }
  state.wirelessTestGate = null;
  assert.deepEqual(testedWireless.at(-1), { ssid: "new-network", password: "new-passphrase" });
  elements.get("#wifi-ssid").value = "edited-network";
  handlers.get("#wifi-ssid:input")();
  elements.get("#wifi-password").value = "edited-passphrase";
  handlers.get("#wifi-password:input")();
  assert.equal(elements.get("#settings-save").disabled, true,
    "editing either credential invalidates the previously tested stage");
  await handlers.get("#wifi-connect:click")();
  assert.deepEqual(testedWireless.at(-1), { ssid: "edited-network", password: "edited-passphrase" });
  assert.equal(elements.get("#settings-save").disabled, false);
  assert.equal(elements.get("#settings-save").disabled, false, "staged wireless settings enable combined save");
  configureRadio("ble");
  handlers.get("#ble-generate:click")();
  assert.match(elements.get("#ble-secret").value, /^[0-9a-f]{64}$/);
  handlers.get("#settings-save:click")();
  assert.equal(elements.get("#configuration-wireless-warning").hidden, false);
  handlers.get("#cancel-configuration:click")();
  await handlers.get("#confirm-configuration:click")();
  assert.equal(wirelessSettings.length, 0);
  const settingsBefore = savedSettings.length;
  state.wirelessFailure = 409;
  await saveAndConfirm();
  assert.equal(wirelessSettings.length, 1);
  assert.match(elements.get("#wifi-status").textContent, /Storage busy/);
  assert.match(elements.get("#settings-note").textContent, /Save failed: Storage busy/);
  assert.equal(elements.get("#settings-save").disabled, true, "a rejected key save requires regeneration before retry");
  assert.match(elements.get("#settings-save").title, /Generate a replacement/);
  assert.equal(elements.get("#wifi-password").value, "");
  assert.equal(elements.get("#ble-secret").value, "");
  state.wirelessFailure = 0;
  handlers.get("#ble-generate:click")();
  assert.equal(elements.get("#settings-save").disabled, false, "regeneration makes the rejected save retryable");
  handlers.get("#settings-save:click")();
  const saving = handlers.get("#confirm-configuration:click")();
  assert.equal(elements.get("#board-select").disabled, true);
  assert.equal(elements.get("#rescan-boards").disabled, true);
  await saving;
  assert.equal(wirelessSettings.length, 2);
  assert.equal(savedSettings.length, settingsBefore, "one combined request, no separate settings save");
  assert.equal(requestedUrls.at(-1), "/api/wireless?board=board-b");
  assert.equal(wirelessSettings[1].confirm_reboot, true);
  assert.equal(wirelessSettings[1].wifi_enabled, true);
  assert.equal(wirelessSettings[1].ble_enabled, true);
  assert.equal(wirelessSettings[1].use_tested_wifi, true);
  assert.equal(wirelessSettings[1].ssid, "edited-network");
  assert.equal(Object.hasOwn(wirelessSettings[1], "password"), false);
  assert.deepEqual(wirelessSettings[1].board, { system_clock_mhz: 133, sdcard_profile: 0, radios: 3 });
  assert.match(elements.get("#wifi-status").textContent, /saved/);
  assert.match(elements.get("#settings-note").textContent, /Settings saved. Reboot requested/);
  assert.equal(elements.get("#settings-save").disabled, true);
  await reconnect();
  state.wirelessFailure = 503;
  configureRadio("wifi");
  handlers.get("#wifi-other:click")();
  elements.get("#wifi-ssid").value = "uncertain-network";
  handlers.get("#wifi-ssid:input")();
  elements.get("#wifi-password").value = "new-passphrase";
  handlers.get("#wifi-password:input")();
  await handlers.get("#wifi-connect:click")();
  await saveAndConfirm();
  assert.equal(wirelessSettings.length, 3);
  assert.match(elements.get("#wifi-status").textContent, /uncertain/);
  await state.tick();
  await saveAndConfirm();
  assert.equal(wirelessSettings.length, 3);
  elements.get("#board-select").value = "board-a";
  handlers.get("#board-select:change")();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-password").value, "");
  assert.equal(elements.get("#ble-secret").value, "");
  const firstBoard = boardStatuses.get("board-a");
  state.wirelessStatus = null;
  firstBoard.hardware.capabilities |= 128;
  firstBoard.storage_stale = false;
  await state.tick();
  state.wirelessFailure = 409;
  state.wirelessError = "Wireless outcome is uncertain";
  configureRadio("wifi");
  handlers.get("#wifi-other:click")();
  elements.get("#wifi-ssid").value = "mismatched-network";
  handlers.get("#wifi-ssid:input")();
  elements.get("#wifi-password").value = "new-passphrase";
  handlers.get("#wifi-password:input")();
  await handlers.get("#wifi-connect:click")();
  await saveAndConfirm();
  assert.equal(wirelessSettings.length, 4);
  assert.match(elements.get("#wifi-status").textContent, /Wireless outcome is uncertain/);
  await state.tick();
  await saveAndConfirm();
  assert.equal(wirelessSettings.length, 4);
  elements.get("#board-select").value = "board-b";
  handlers.get("#board-select:change")();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(elements.get("#wifi-connect").disabled, true, "board B retains its earlier unresolved save");
  connected.command_outcome_uncertain = true;
  connected.command_resync_supported = true;
  await state.tick();
  await handlers.get("#command-resync:click")();
  configureRadio("wifi");
  elements.get("#wifi-ssid").value = "timed-out-network";
  handlers.get("#wifi-ssid:input")();
  state.wirelessTestError = new DOMException("HTTP deadline", "TimeoutError");
  const beforeTimeout = testedWireless.length;
  await handlers.get("#wifi-connect:click")();
  assert.equal(testedWireless.length, beforeTimeout + 1, "timed-out tests are not replayed");
  assert.match(elements.get("#wifi-status").textContent, /timed out.*uncertain/);
  await state.tick();
  assert.equal(elements.get("#wifi-connect").disabled, true);
  assert.equal(elements.get("#settings-save").disabled, true);
  connected.command_outcome_uncertain = true;
  connected.command_resync_supported = true;
  await state.tick();
  assert.equal(elements.get("#command-resync").hidden, false);
  await handlers.get("#command-resync:click")();
  assert.equal(connected.command_outcome_uncertain, false);
  assert.equal(elements.get("#command-resync").hidden, true);
  assert.equal(requestedUrls.at(-1), "/api/status?board=board-b");
});
