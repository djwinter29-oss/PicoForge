import assert from "node:assert/strict";
import test from "node:test";
import { renderWirelessStatus } from "../../../web/js/ui/sections/wireless.mjs";
import { withDocument, snapshot } from "../../support/view.mjs";

test("Direct dashboard shows live Wi-Fi and Bluetooth state without treating unavailable reports as disabled", () =>
  withDocument((el, renderStatus) => {
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

test("keyless Bluetooth reports that a control key is needed; unavailable reports stay distinct", () => {
  const previous = globalThis.document;
  const elements = new Map();
  globalThis.document = { querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, {});
    return elements.get(selector);
  } };
  const snapshot = { connected: true, hardware: { capabilities: 128, serial_number: "AB" }, wireless: {
    wifi_enabled: true, ssid: "saved", wifi_state: "failed", ble_enabled: true, ble_state: "off",
    ble_key_configured: false,
  } };
  try {
    renderWirelessStatus(snapshot);
    assert.equal(elements.get("#direct-ble-state").textContent, "Off (no control key)");
    assert.doesNotMatch(elements.get("#direct-wifi-state").textContent, /key/);
    renderWirelessStatus({ ...snapshot, wireless: { ...snapshot.wireless, ble_key_configured: true } });
    assert.equal(elements.get("#direct-ble-state").textContent, "PicoForge-AB (starting)");
    renderWirelessStatus({ ...snapshot, connected: false });
    assert.equal(elements.get("#direct-wifi-state").textContent, "--");
    renderWirelessStatus({ ...snapshot, wireless_error: "Read failed" });
    assert.equal(elements.get("#direct-wifi-state").textContent, "Unavailable");
    assert.equal(elements.get("#direct-wifi-state").title, "Read failed");
  } finally { globalThis.document = previous; }
});

test("connected Wi-Fi CLI status links to the console in a new tab", () => {
  const previous = globalThis.document;
  const elements = new Map();
  const wifi = { textContent: "", title: "", children: [], querySelector() { return this.children[0] ?? null; },
    replaceChildren(...children) { this.children = children; } };
  const makeElement = () => ({ textContent: "", title: "", hidden: false, dataset: {} });
  elements.set("#direct-wifi-diagnostics", makeElement());
  elements.set("#direct-wifi-state", wifi);
  elements.set("#direct-ble-diagnostics", makeElement());
  elements.set("#direct-ble-state", makeElement());
  globalThis.document = { querySelector: selector => elements.get(selector), createElement: () => ({
    dataset: {}, textContent: "", href: "", title: "",
  }) };
  try {
    renderWirelessStatus({ connected: true, hardware: { capabilities: 128 },
      console_url: "console.html?ipv4=192.168.1.9",
      wireless: { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
        ssid: "HomeDev", ipv4: "192.168.1.9", ble_enabled: false } });
    assert.equal(wifi.children[0].textContent, "HomeDev (192.168.1.9)");
    assert.equal(wifi.children[0].href, "console.html?ipv4=192.168.1.9");
    assert.equal(wifi.children[0].target, "_blank");
    assert.equal(wifi.children[0].rel, "noopener noreferrer");
  } finally { globalThis.document = previous; }
});
