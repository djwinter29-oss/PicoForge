import assert from "node:assert/strict";
import test from "node:test";
import { withDocument, snapshot } from "../../support/view.mjs";

test("Direct dashboard shows live Wi-Fi and Bluetooth state without treating unavailable reports as disabled", () => withDocument((el, renderStatus) => {
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

