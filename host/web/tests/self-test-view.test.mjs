import assert from "node:assert/strict";
import test from "node:test";
import { renderStatus as renderServerStatus, renderSelfTest as renderServerSelfTest } from "../js/dashboard.mjs";

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
      [{ ble_enabled: true, ble_key_configured: false, ble_state: "off" }, "PicoForge-892BA313F9BB56E1 (starting)"],
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
