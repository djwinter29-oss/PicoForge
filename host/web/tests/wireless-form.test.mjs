import assert from "node:assert/strict";
import test from "node:test";
import { bindWirelessForm, renderWirelessStatus } from "../js/wireless-form.mjs";

test("missing TCP auth key shows an actionable hint without misdiagnosing other states", () => {
  const previous = globalThis.document;
  const elements = new Map();
  globalThis.document = { querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, {});
    return elements.get(selector);
  } };
  const snapshot = { connected: true, hardware: { capabilities: 128 }, wireless: {
    wifi_enabled: true, ssid: "saved", wifi_state: "failed", error: 1,
    tcp_cli_auth_required: true, ble_key_configured: false, restart_required: false,
  } };
  try {
    renderWirelessStatus(snapshot);
    assert.match(elements.get("#wifi-state").textContent, /TCP authentication needs a key/);
    assert.match(elements.get("#wifi-state").title, /Save a BLE control key or disable TCP authentication/);
    for (const change of [{ error: 0 }, { error: 2 }, { ble_key_configured: true },
      { tcp_cli_auth_required: false }, { restart_required: true }]) {
      renderWirelessStatus({ ...snapshot, wireless: { ...snapshot.wireless, ...change } });
      assert.doesNotMatch(elements.get("#wifi-state").textContent, /needs a key/);
    }
    renderWirelessStatus({ ...snapshot, connected: false });
    assert.equal(elements.get("#wifi-state").textContent, "--");
    renderWirelessStatus({ ...snapshot, wireless_error: "Read failed" });
    assert.equal(elements.get("#wifi-state").textContent, "Unavailable");
    assert.equal(elements.get("#wifi-state").title, "Read failed");
  } finally { globalThis.document = previous; }
});

test("TCP switch is saved only on v2 and expired test credentials block saving", async context => {
  const previous = globalThis.document;
  const elements = new Map();
  const el = id => {
    if (!elements.has(id)) elements.set(id, {
      value: "", children: [], handlers: {},
      label: { hidden: true, toggleAttribute(name, hidden) { this[name] = hidden; } },
      closest() { return this.label; },
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
  let supported = true, ready = true;
  try {
    el("settings-wifi").value = "1";
    el("settings-ble").value = "0";
    const form = bindWirelessForm({ load: async () => status, ready: () => ready, supported: () => supported,
      test: async () => ({ ipv4: "192.168.1.2", remaining_seconds: 600 }), cancelTest: async () => {} });
    await form.load();
    assert.equal(form.dirty(), false);
    assert.equal(el("settings-tcp-cli").disabled, false);
    assert.equal(el("settings-tcp-cli").label.hidden, false);
    assert.equal(el("settings-tcp-auth").label.hidden, false);
    assert.equal(el("settings-tcp-auth").disabled, true, "authentication needs a configured or staged BLE key");
    el("settings-tcp-cli").value = "0";
    el("settings-tcp-cli").handlers.change();
    await form.load();
    assert.equal(el("settings-tcp-cli").value, "0", "periodic refresh preserves unsaved TCP edits");
    assert.equal(form.dirty(), true);
    assert.equal(form.request().tcp_cli_enabled, false);
    form.finish({ ...status, tcp_cli_enabled: false });
    assert.equal(form.dirty(), false);
    Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues: bytes => bytes.fill(1) } });
    el("ble-generate").handlers.click();
    assert.equal(el("settings-tcp-auth").disabled, true, "authentication is greyed out while TCP CLI is off");
    el("settings-tcp-cli").value = "1";
    el("settings-tcp-cli").handlers.change();
    assert.equal(el("settings-tcp-auth").disabled, false);
    el("settings-tcp-auth").value = "1";
    el("settings-tcp-auth").handlers.change();
    assert.equal(form.request().tcp_cli_auth_required, true);
    el("settings-ble").value = "0";
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, false, "TCP CLI only depends on Wi-Fi");
    assert.equal(el("settings-tcp-auth").disabled, true, "authentication depends on Bluetooth");
    assert.equal(el("settings-tcp-auth").value, "1", "greying out preserves the selection");
    assert.equal(form.request().tcp_cli_auth_required, false, "disabling Bluetooth disables auth on save");
    el("settings-ble").value = "1";
    form.update();
    assert.equal(el("settings-tcp-auth").disabled, false);
    assert.equal(form.request().tcp_cli_auth_required, true);
    ready = false;
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, true);
    assert.equal(el("settings-tcp-auth").disabled, true);
    ready = true;
    form.update();
    el("wifi-ssid").value = "tested";
    await el("wifi-connect").handlers.click();
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(form.canSave(), true);
    el("wifi-ssid").value = "edited-network";
    el("wifi-ssid").handlers.input();
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /test the connection again/);
    el("wifi-password").value = "new-passphrase";
    el("wifi-password").handlers.input();
    assert.equal(form.canSave(), false);
    await el("wifi-connect").handlers.click();
    assert.equal(form.request().ssid, "edited-network");
    assert.equal(form.request().use_tested_wifi, true);
    now += 600000;
    form.update();
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /expired/);
    assert.match(el("wireless-summary").textContent, /expired/);
    el("settings-wifi").value = "0";
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, true);
    assert.equal(el("settings-tcp-auth").disabled, true);
    assert.equal(el("settings-tcp-cli").value, "1");
    assert.equal(form.canSave(), true, "expired credentials do not prevent disabling Wi-Fi");
    const disabled = form.request();
    assert.equal(disabled.wifi_enabled, false);
    assert.equal(disabled.password, "");
    assert.equal(disabled.ssid, "");
    assert.equal(Object.hasOwn(disabled, "use_tested_wifi"), false);
    assert.equal(el("wireless-summary").hidden, true);
    form.reset();
    form.update();
    assert.equal(el("settings-tcp-auth").label.hidden, false, "W boards show auth before status loads (e.g. BLE)");
    assert.equal(el("settings-tcp-auth").disabled, true, "auth stays greyed until status confirms support");
    status = { ...status, tcp_cli_configurable: false };
    await form.load();
    assert.equal(el("settings-tcp-cli").disabled, true);
    assert.equal(Object.hasOwn(form.request(), "tcp_cli_enabled"), false);
    supported = false;
    form.update();
    for (const id of ["settings-tcp-cli", "settings-tcp-auth"]) {
      assert.equal(el(id).label.hidden, true, "non-wireless boards hide TCP controls");
      assert.equal(el(id).disabled, true);
    }
  } finally {
    globalThis.document = previous;
    if (previousCrypto) Object.defineProperty(globalThis, "crypto", previousCrypto);
    else delete globalThis.crypto;
  }
});

test("failed temporary Wi-Fi cancellation retains the stage and permits a safe retry", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const el = id => {
    if (!elements.has(id)) elements.set(id, {
      value: id === "settings-wifi" ? "1" : "", textContent: "", children: [], handlers: {},
      options: ["0", "1", "configure"].map(value => ({ value })),
      label: { hidden: false, toggleAttribute(name, hidden) { this[name] = hidden; } },
      closest() { return this.label; },
      addEventListener(event, callback) { this.handlers[event] = callback; },
      replaceChildren(...children) { this.children = children; },
      append(...children) { this.children.push(...children); },
      showModal() { this.open = true; },
      close() { this.open = false; },
    });
    return elements.get(id);
  };
  globalThis.document = {
    querySelector: selector => el(selector.slice(1)),
    createElement: () => ({ children: [], append(...children) { this.children.push(...children); } }),
  };
  let cancelCalls = 0;
  let cancelFailure = "busy";
  try {
    const form = bindWirelessForm({
      load: async () => ({ ssid: "saved", wifi_enabled: true, ble_key_configured: false }),
      scan: async () => [],
      test: async () => ({ ipv4: "192.168.1.2", remaining_seconds: 600 }),
      cancelTest: async () => {
        cancelCalls++;
        if (cancelFailure === "busy") throw new Error("device is busy");
        if (cancelFailure === "timeout") throw new DOMException("request timed out", "TimeoutError");
      },
      ready: () => true,
      supported: () => true,
    });
    await form.load();
    el("wifi-ssid").value = "tested-network";
    await el("wifi-connect").handlers.click();
    el("wifi-dialog").showModal();
    await el("wifi-cancel").handlers.click();

    assert.equal(cancelCalls, 1);
    assert.equal(el("wifi-dialog").open, true, "keep the dialog open when cleanup fails");
    assert.equal(el("wifi-cancel").textContent, "Retry clearing test");
    assert.match(el("wifi-status").textContent, /device is busy/);
    assert.match(el("wireless-summary").textContent, /Open Wi-Fi settings to retry/);
    assert.equal(form.stageValid(), false);
    assert.equal(form.canSave(), false);
    assert.equal(form.operationBusy(), true, "board selection stays locked until temporary credentials are cleared");
    assert.equal(el("wifi-connect").disabled, true, "block another wireless operation until cleanup succeeds");
    assert.throws(() => form.request(), /Clear the temporary Wi-Fi test/);

    cancelFailure = "";
    await el("wifi-cancel").handlers.click();
    assert.equal(cancelCalls, 2);
    assert.equal(el("wifi-dialog").open, false);
    assert.equal(form.stageValid(), true);
    assert.equal(Object.hasOwn(form.request(), "use_tested_wifi"), false);
    assert.equal(el("wireless-summary").hidden, true);

    el("wifi-ssid").value = "second-network";
    await el("wifi-connect").handlers.click();
    el("wifi-dialog").showModal();
    cancelFailure = "timeout";
    await el("wifi-cancel").handlers.click();
    assert.equal(form.operationBusy(), true, "unknown cleanup outcomes keep board operations locked");
    assert.match(el("wireless-summary").textContent, /Reload the dashboard and reconnect the board/);
    assert.equal(el("wifi-dialog").open, true);
    form.reset();
  } finally {
    globalThis.document = previous;
  }
});
