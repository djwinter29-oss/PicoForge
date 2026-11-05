import assert from "node:assert/strict";
import test from "node:test";
import { bindWirelessForm, renderWirelessStatus } from "../js/wireless-form.mjs";

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
    assert.equal(elements.get("#ble-state").textContent, "Off (no control key)");
    assert.doesNotMatch(elements.get("#wifi-state").textContent, /key/);
    renderWirelessStatus({ ...snapshot, wireless: { ...snapshot.wireless, ble_key_configured: true } });
    assert.equal(elements.get("#ble-state").textContent, "PicoForge-AB (starting)");
    renderWirelessStatus({ ...snapshot, connected: false });
    assert.equal(elements.get("#wifi-state").textContent, "--");
    renderWirelessStatus({ ...snapshot, wireless_error: "Read failed" });
    assert.equal(elements.get("#wifi-state").textContent, "Unavailable");
    assert.equal(elements.get("#wifi-state").title, "Read failed");
  } finally { globalThis.document = previous; }
});

test("scan selection retains raw SSID bytes, manual edits drop hex, "
  + "and failed-test cancellation clears staging", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const makeElement = () => ({
    value: "", children: [], handlers: {}, options: ["0", "1", "configure"].map(value => ({ value })),
    label: { toggleAttribute() {} }, closest() { return this.label; },
    setAttribute() {}, append(...items) { this.children.push(...items); },
    addEventListener(event, callback) { this.handlers[event] = callback; },
    replaceChildren(...items) { this.children = items; }, close() {}, showModal() {}, focus() {},
  });
  const el = id => {
    if (!elements.has(id)) elements.set(id, makeElement());
    return elements.get(id);
  };
  globalThis.document = { querySelector: selector => el(selector.slice(1)), createElement: makeElement };
  let cancelCount = 0, cancellationFails = false, testFails = false;
  const tested = [];
  const form = bindWirelessForm({
    supported: () => true, ready: () => true,
    load: async () => ({ ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true, ble_enabled: false }),
    scan: async () => [{ ssid: "\\x00\\xFF<script>", ssid_hex: "00ff3c7363726970743e", rssi: -60, secured: false }],
    test: async request => {
      tested.push(request);
      if (testFails) throw new Error("Password rejected");
      return { ipv4: "192.168.1.2", remaining_seconds: 599 };
    },
    cancelTest: async () => { cancelCount++; if (cancellationFails) throw new Error("clear failed"); },
  });
  try {
    el("settings-wifi").value = "1"; el("settings-ble").value = "0";
    await form.load();
    await el("wifi-scan").handlers.click();
    const button = el("wifi-networks").children[0];
    assert.equal(button.children[0].textContent, "\\x00\\xFF<script>", "SSID label is text, never HTML");
    button.handlers.click();
    await el("wifi-connect").handlers.click();
    assert.equal(tested[0].ssid_hex, "00ff3c7363726970743e");
    assert.equal(form.request().ssid_hex, "00ff3c7363726970743e");
    assert.equal(form.request().use_tested_wifi, true);
    cancellationFails = true;
    await el("wifi-cancel").handlers.click();
    assert.equal(form.canSave(), false, "failed discard cannot silently lose the retained pair");
    cancellationFails = false;
    await el("wifi-cancel").handlers.click();
    assert.equal(form.canSave(), true);
    assert.equal(cancelCount, 2);
    el("wifi-ssid").value = "manual"; el("wifi-ssid").handlers.input();
    testFails = true;
    await el("wifi-connect").handlers.click();
    assert.equal(Object.hasOwn(tested[1], "ssid_hex"), false, "manual text is encoded exactly once");
    await el("wifi-cancel").handlers.click();
    assert.equal(cancelCount, 3, "a failed test also leaves firmware staging that must be discarded");
  } finally { form.reset(); globalThis.document = previous; }
});

test("connected Wi-Fi CLI status links to the console in a new tab", () => {
  const previous = globalThis.document;
  const elements = new Map();
  const wifi = { textContent: "", title: "", children: [], querySelector() { return this.children[0] ?? null; },
    replaceChildren(...children) { this.children = children; } };
  const makeElement = () => ({ textContent: "", title: "", hidden: false, dataset: {} });
  elements.set("#wifi-diagnostics", makeElement());
  elements.set("#wifi-state", wifi);
  elements.set("#ble-diagnostics", makeElement());
  elements.set("#ble-state", makeElement());
  globalThis.document = { querySelector: selector => elements.get(selector), createElement: () => ({
    dataset: {}, textContent: "", href: "", title: "",
  }) };
  try {
    renderWirelessStatus({ connected: true, hardware: { capabilities: 128 },
      console_url: "console.html?ipv4=192.168.1.9&auth=1",
      wireless: { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
        ssid: "HomeDev", ipv4: "192.168.1.9", ble_enabled: false } });
    assert.equal(wifi.children[0].textContent, "HomeDev (192.168.1.9)");
    assert.equal(wifi.children[0].href, "console.html?ipv4=192.168.1.9&auth=1");
    assert.equal(wifi.children[0].target, "_blank");
    assert.equal(wifi.children[0].rel, "noopener noreferrer");
  } finally { globalThis.document = previous; }
});

test("TCP switch is saved explicitly and expired test credentials block saving", async context => {
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
  let status = { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true,
    ble_key_configured: false };
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
    el("settings-tcp-cli").value = "0";
    el("settings-tcp-cli").handlers.change();
    await form.load();
    assert.equal(el("settings-tcp-cli").value, "0", "periodic refresh preserves unsaved TCP edits");
    assert.equal(form.dirty(), true);
    assert.equal(form.request().tcp_cli_enabled, false);
    form.finish({ ...status, tcp_cli_enabled: false });
    assert.equal(form.dirty(), false);
    const bleOn = () => el("settings-ble").options.find(option => option.value === "1");
    assert.equal(bleOn().disabled, true, "Bluetooth cannot be enabled without a saved or staged key");
    Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues: bytes => bytes.fill(1) } });
    el("ble-generate").handlers.click();
    assert.equal(bleOn().disabled, false);
    el("settings-tcp-cli").value = "1";
    el("settings-tcp-cli").handlers.change();
    assert.equal(el("settings-ble").value, "1", "generating a key enables Bluetooth");
    assert.equal(Object.hasOwn(form.request(), "tcp_cli_auth_required"), false, "TCP auth follows the key");
    el("settings-ble").value = "0";
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, false, "TCP CLI only depends on Wi-Fi");
    ready = false;
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, true);
    ready = true;
    form.update();
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
    assert.equal(el("settings-tcp-cli").disabled, true);
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
    supported = false;
    form.update();
    for (const id of ["settings-tcp-cli"]) {
      assert.equal(el(id).label.hidden, true, "non-wireless boards hide TCP controls");
      assert.equal(el(id).disabled, true);
    }
  } finally {
    globalThis.document = previous;
    if (previousCrypto) Object.defineProperty(globalThis, "crypto", previousCrypto);
    else delete globalThis.crypto;
  }
});
