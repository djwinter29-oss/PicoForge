import assert from "node:assert/strict";
import test from "node:test";
import { bindWirelessForm } from "../js/wireless-form.mjs";

test("TCP switch is saved only on v2 and expired test credentials block saving", async context => {
  const previous = globalThis.document;
  const elements = new Map();
  const el = id => {
    if (!elements.has(id)) elements.set(id, {
      value: "", checked: true, children: [], handlers: {},
      options: ["0", "1", "configure"].map(value => ({ value })),
      addEventListener(event, callback) { this.handlers[event] = callback; },
      replaceChildren() {}, close() {}, showModal() {},
    });
    return elements.get(id);
  };
  globalThis.document = { querySelector: selector => el(selector.slice(1)) };
  let now = 100;
  const previousCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  context.mock.method(Date, "now", () => now);
  let status = { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true, tcp_cli_configurable: true,
    ble_key_configured: false, tcp_cli_auth_required: false, tcp_cli_auth_configurable: true };
  try {
    el("settings-wifi").value = "1";
    el("settings-ble").value = "0";
    el("settings-tcp-auth").checked = false;
    const form = bindWirelessForm({ load: async () => status, ready: () => true, supported: () => true,
      test: async () => ({ ipv4: "192.168.1.2", remaining_seconds: 600 }), cancelTest: async () => {} });
    await form.load();
    assert.equal(form.dirty(), false);
    assert.equal(el("settings-tcp-auth").disabled, true, "authentication needs a configured or staged BLE key");
    el("settings-tcp-cli").checked = false;
    form.update();
    await form.load();
    assert.equal(el("settings-tcp-cli").checked, false, "periodic refresh preserves unsaved TCP edits");
    assert.equal(form.dirty(), true);
    assert.equal(form.request().tcp_cli_enabled, false);
    form.finish({ ...status, tcp_cli_enabled: false });
    assert.equal(form.dirty(), false);
    Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues: bytes => bytes.fill(1) } });
    el("ble-generate").handlers.click();
    assert.equal(el("settings-tcp-auth").disabled, false);
    el("settings-tcp-auth").checked = true;
    form.update();
    assert.equal(form.request().tcp_cli_auth_required, true);
    el("wifi-ssid").value = "tested";
    await el("wifi-connect").handlers.click();
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(form.canSave(), true);
    now += 600000;
    form.update();
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /expired/);
    assert.match(el("wireless-summary").textContent, /expired/);
    el("settings-wifi").value = "0";
    form.update();
    assert.equal(form.canSave(), true, "expired credentials do not prevent disabling Wi-Fi");
    const disabled = form.request();
    assert.equal(disabled.wifi_enabled, false);
    assert.equal(disabled.password, "");
    assert.equal(disabled.ssid, "");
    assert.equal(Object.hasOwn(disabled, "use_tested_wifi"), false);
    assert.equal(el("wireless-summary").hidden, true);
    form.reset();
    status = { ...status, tcp_cli_configurable: false };
    await form.load();
    assert.equal(el("settings-tcp-cli").disabled, true);
    assert.equal(Object.hasOwn(form.request(), "tcp_cli_enabled"), false);
  } finally {
    globalThis.document = previous;
    if (previousCrypto) Object.defineProperty(globalThis, "crypto", previousCrypto);
    else delete globalThis.crypto;
  }
});
