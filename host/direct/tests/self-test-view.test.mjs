import assert from "node:assert/strict";
import test from "node:test";
import { createDashboardView } from "../web/js/ui/dashboard-view.mjs";
import { renderSelfTest } from "../web/js/ui/sections/self-test.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../web/js/protocol/codec.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

let renderStatus;
function withDocument(run) {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", value: "0", open: false,
      valueAsNumber: 32, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; this.replaceCount = (this.replaceCount ?? 0) + 1; } });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({ children: [],
    append(...children) { this.children.push(...children); } }) };
  renderStatus = createDashboardView();
  try { run(element); } finally { globalThis.document = saved; }
}

function snapshot() {
  const hardware = decodeIdentityReport(identity());
  return { connected: true, hardware, settings: decodeConfigReport(config(), hardware),
    freshness: { 3: "fresh", 10: "fresh", 11: "fresh" }, self_test: decodeSelfTestReport(selfTest()),
    memory: { sample_valid: true, initialized: true, configured: true, presence_known: true, present: true },
    msc: { enabled: false, transition_pending: false }, filesystem_format: "exfat" };
}

test("board pin tables are reused until the displayed configuration changes", () => withDocument(el => {
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

test("Direct dashboard shows live Wi-Fi and Bluetooth state without treating unavailable reports as disabled", () => withDocument(el => {
  for (const render of [renderStatus]) {
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
      assert.equal(el("#direct-wifi-state").textContent, expected);
      assert.equal(el("#direct-wifi-diagnostics").hidden, false);
    }
    for (const [wireless, expected] of [
      [{ wifi_enabled: true, ssid: "", wifi_state: "connected", ipv4: "0.0.0.0" }, "Disabled"],
      [{ wifi_enabled: true, ssid: "", wifi_state: "connecting" }, "Disabled"],
      [{ wifi_enabled: false, ssid: "", wifi_state: "disabled" }, "Disabled"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "connected", ipv4: "192.168.66.87" },
        "HomeDev (192.168.66.87)"],
      [{ wifi_enabled: true, ssid: "HomeDev", wifi_state: "connecting" }, "HomeDev (connecting)"],
    ]) {
      render({ ...state, link: "ble", wireless });
      assert.equal(el("#direct-wifi-state").textContent, expected);
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
      assert.equal(el("#direct-ble-state").textContent, expected);
      assert.equal(el("#direct-ble-diagnostics").hidden, false);
    }
    render({ ...state, hardware: { capabilities: 128 },
      wireless: { ble_enabled: true, ble_state: "connected" } });
    assert.equal(el("#direct-ble-state").textContent, "PicoForge (connected)");
    render({ ...state, wireless_error: "Wi-Fi status read failed: disconnected" });
    assert.equal(el("#direct-ble-state").textContent, "Unavailable");
    assert.equal(el("#direct-wifi-state").textContent, "Unavailable");
    assert.match(el("#direct-wifi-state").title, /read failed/);
    render({ ...state, connected: false });
    assert.equal(el("#direct-wifi-state").textContent, "--");
    render({ ...state, hardware: { capabilities: 16 } });
    assert.equal(el("#direct-wifi-diagnostics").hidden, true);
    assert.equal(el("#direct-ble-diagnostics").hidden, true);
  }
}));

test("self-test writes require fresh, known, mounted, firmware-owned state and valid size", () => withDocument(el => {
  const state = snapshot();
  renderSelfTest(state);
  assert.equal(el("#self-test-start").disabled, false);
  for (const mutate of [
    value => { value.freshness[10] = "stale"; },
    value => { value.freshness[11] = "unavailable"; },
    value => { value.hardware.capabilities &= ~32; },
    value => { value.self_test.valid_for_control = false; },
    value => { value.self_test.volume_mounted = false; },
    value => { value.msc.enabled = true; },
    value => { value.msc.transition_pending = true; },
    value => { value.pending_msc = true; },
    value => { value.pending_self_test = { sequence: 0 }; },
    value => { value.outcome_uncertain = true; },
    value => { value.busy = true; },
    value => { value.link = "ble"; },
  ]) {
    const changed = snapshot(); mutate(changed);
    renderSelfTest(changed);
    assert.equal(el("#self-test-start").disabled, true);
  }
  for (const size of [0, 1025, 1.5, NaN]) {
    el("#self-test-size").valueAsNumber = size;
    renderSelfTest(state); assert.equal(el("#self-test-start").disabled, true);
  }
  el("#self-test-size").valueAsNumber = 1024;
  renderSelfTest(state); assert.equal(el("#self-test-start").disabled, false);
  state.self_test = { ...state.self_test, state: "writing", active: true };
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, true);
  assert.equal(el("#self-test-cancel").disabled, false);
  assert.equal(el("#self-test-start").disabled, true);
  state.self_test.state = "cleanup"; renderSelfTest(state);
  assert.equal(el("#self-test-cancel").disabled, true);
}));

test("self-test presents retained throughput and unknown enums without obsolete request errors",
  () => withDocument(el => {
  const state = snapshot();
  state.self_test = decodeSelfTestReport(selfTest(6));
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);
  assert.equal(el("#self-test-state").textContent, "Passed · 100%");
  assert.equal(el("#self-test-meter").dataset.result, "passed");
  assert.equal(el("#self-test-write-speed").textContent, "2.00 MiB/s");
  assert.equal(el("#self-test-read-speed").textContent, "4.00 MiB/s");
  assert.equal(el("#self-test-progress").value, 100);
  assert.equal(el("#self-test-message").textContent, "");
  state.self_test = decodeSelfTestReport(selfTest(99));
  renderSelfTest(state);
  assert.match(el("#self-test-state").textContent, /^Unknown state \(99\) · \d+%$/);
  assert.equal(el("#self-test-meter").dataset.result, "");
  assert.match(el("#self-test-message").textContent, /Unknown status values/);
  assert.equal(el("#self-test-start").disabled, true);
  state.self_test = { ...state.self_test, state: "failed", active: false };
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, true);
  el("#self-test-disclosure").open = false;
  renderSelfTest(state);
  assert.equal(el("#self-test-disclosure").open, false);
}));

test("identity RAM stays build accounting while stale inputs show no current values", () => withDocument(el => {
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

test("storage does not infer absence from an unavailable sample or mount from cached format", () => withDocument(el => {
  const state = snapshot();
  state.memory.sample_valid = false; state.memory.presence_known = false; state.memory.initialized = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="memory-title"]').hidden, false);
  assert.equal(el("#memory-note").textContent, "");
  assert.equal(el("#memory-state").textContent, "Busy");
  state.memory.sample_valid = true; state.memory.presence_known = true; state.memory.present = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="memory-title"]').hidden, true);
  state.hardware.capabilities &= ~32;
  state.memory.presence_known = false;
  renderStatus(state);
  assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
}));

test("Direct dashboard retains storage visibility through stale reports without showing cached readings", () =>
  withDocument(el => {
    for (const render of [renderStatus]) {
      render({ connected: false });
      const state = snapshot();
      state.memory.present = false;
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, true);
      assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);

      state.memory = null;
      state.msc = null;
      state.storage_stale = true;
      state.freshness[10] = "stale";
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, true, "A missing report must not reveal absent storage");
      assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);

      state.memory = { sample_valid: true, presence_known: true, present: true, initialized: true, configured: true };
      state.storage_stale = false;
      state.freshness[10] = "fresh";
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false, "Confirmed card insertion must update layout");
      state.memory = null;
      state.storage_stale = true;
      state.freshness[10] = "stale";
      render(state);
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(el("#memory-size").textContent, "--", "Stale readings must not be presented as live");

      render({ connected: false });
      assert.equal(el('[aria-labelledby="memory-title"]').hidden, false, "Disconnect must clear confirmed presence");
    }
  }));

test("busy storage retains card details without a message, but stale or replaced media clears them", () =>
  withDocument(el => {
    const state = snapshot();
    state.memory = { ...state.memory, mode: "sdio", bus_clock_hz: 50000000, speed_mode: 2,
      capacity_bytes: 127865454592, manufacturer: { id: 3, product_name: "SB128", product_revision: "8.0",
        manufacture_year: 2020, manufacture_month: 1, serial_number: 4089330880 } };
    const busy = { ...state, memory: { configured: true, sample_valid: false } };
    renderStatus(state);
    const ids = ["#memory-speed", "#memory-size", "#memory-filesystem",
      "#memory-manufacturer", "#memory-product", "#memory-serial"];
    const details = ids.map(id => el(id).textContent);
    assert.equal(el("#memory-speed").textContent, "50 MHz");
    assert.equal(el("#storage-freshness").hidden, true);
    for (let sample = 0; sample < 3; sample++) {
      renderStatus(busy);
      assert.equal(el("#memory-state").textContent, "Busy");
      assert.equal(el("#memory-note").hidden, true);
      assert.deepEqual(ids.map(id => el(id).textContent), details);
    }
    for (const invalidation of [
      { ...state, connected: false, memory: null },
      { ...state, memory: null, freshness: { 10: "stale" } },
      { ...state, memory: { configured: true, sample_valid: true, initialized: false, present: false } },
      { ...state, msc: { enabled: true } },
      { ...state, msc: { transition_pending: true } },
    ]) {
      renderStatus(state);
      renderStatus(invalidation);
      renderStatus(busy);
      assert.equal(el("#memory-speed").textContent, "--");
    }
  }));

test("SPI pin table has two rows and keeps card detect after the four bus signals", () => withDocument(el => {
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
