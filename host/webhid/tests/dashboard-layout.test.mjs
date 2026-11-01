import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { PicoForgeWebHid } from "../js/device.mjs?v=5";
import { PicoForgeBluetooth } from "../js/bluetooth.mjs?v=3";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../js/device.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

test("standalone dashboard labels profiles, active measurements, controls and accessible statuses", () => {
  const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>PicoForge Dashboard \| WebHID<\/title>/);
  assert.match(html, /rel="icon"[^>]*href="favicon\.svg\?v=2"/);
  assert.match(html, /href="css\/webhid\.css\?v=12"/);
  assert.match(html, /src="js\/app\.mjs\?v=20"/);
  const app = readFileSync(new URL("../js/app.mjs", import.meta.url), "utf8");
  assert.match(app, /from "\.\/device\.mjs\?v=5"/);
  assert.match(app, /from "\.\/view\.mjs\?v=9"/);
  assert.doesNotMatch(html, /id="connection-control"|id="connection-transport"/);
  for (const name of ["usb", "bluetooth"]) {
    assert.match(html, new RegExp(`id="${name}-open"[\\s\\S]*?aria-haspopup="dialog"`));
    assert.match(html, new RegExp(`<dialog[^>]*id="${name}-connection-dialog"[\\s\\S]*?aria-labelledby=`));
    assert.match(html, new RegExp(`id="${name}-connection-status" role="status" aria-live="polite"`));
  }
  assert.match(html, /id="bluetooth-key-step" hidden/);
  assert.match(html, /<label for="settings-storage">SD Card/);
  assert.match(html, /value="0">Standard/);
  assert.match(html, /value="1">High-Speed/);
  assert.match(html, /id="settings-save"[^>]*>Save and Reboot/);
  assert.match(html, /id="settings-reboot"[^>]*>Reboot/);
  for (const path of ["../index.html", "../../web/index.html"]) {
    const dashboard = readFileSync(new URL(path, import.meta.url), "utf8");
    const settings = dashboard.split('aria-labelledby="settings-title"')[1].split("</section>")[0];
    const footer = dashboard.split('<footer class="status-line"')[1].split("</footer>")[0];
    assert.doesNotMatch(settings, /id="settings-note"/);
    assert.match(settings, /id="wireless-actions" hidden/);
    assert.match(settings, /id="wifi-open"[^>]*>Configure Wi-Fi/);
    assert.match(settings, /id="ble-open"[^>]*>Configure Bluetooth/);
    const board = settings.split('<div class="settings-controls">')[1].split("</div>")[0];
    assert.deepEqual([...board.matchAll(/<label for="([^"]+)"/g)].map(match => match[1]),
      ["settings-clock", "settings-storage", "settings-wifi", "settings-ble"]);
    assert.match(board, /<label for="settings-wifi" hidden>[\s\S]*<label for="settings-ble" hidden>/);
    assert.doesNotMatch(settings, /id="wifi-ssid"|id="wifi-password"|id="ble-secret"/);
    assert.match(dashboard, /<dialog[^>]*id="wifi-dialog"/);
    assert.match(dashboard, /<dialog[^>]*id="ble-dialog"/);
    assert.doesNotMatch(dashboard, /id="wireless-save"/);
    assert.match(dashboard, /id="configuration-wireless-warning" hidden/);
    assert.match(footer, /role="status" aria-live="polite"/);
    assert.match(footer, /id="status-message"[\s\S]*id="settings-note"/);
    assert.equal([...dashboard.matchAll(/id="settings-note"/g)].length, 1);
    assert.match(dashboard, /id="self-test-start"[^>]*>Start benchmark<\/button>/);
    assert.doesNotMatch(dashboard, />Start self-test<\/button>/);
    assert.match(dashboard, /<details class="pin-map-disclosure self-test-disclosure" id="self-test-disclosure">/);
    assert.match(dashboard, /<summary>\s*<h2[^>]*>Storage benchmark<\/h2>\s*<span[^>]*id="self-test-state"/);
    assert.match(dashboard, /<\/button>\s*<\/div>\s*<progress id="self-test-progress"/);
    const benchmark = dashboard.split('aria-labelledby="self-test-title"')[1].split("</section>")[0];
    assert.deepEqual([...benchmark.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
      ["Written / target", "Write duration", "Average write speed", "Verified / target", "Read duration",
        "Average read speed"]);
  }
  assert.doesNotMatch(html, /Save &amp; Reboot|25 MHz|50 MHz|Core 1 \/ Core 2/);
  assert.match(html, /Firmware RAM reserved \/ total/);
  assert.match(html, /Service load \(Core 0 \/ Core 1\)/);
  assert.match(html, /<dt>Filesystem<\/dt>/);
  assert.doesNotMatch(html, /class="eyebrow"|id="mcu"|id="memory-timing"|id="board-config"/);
  assert.doesNotMatch(html, /metric-product/);
  assert.match(html, /id="settings-title">Board configuration/);
  const identity = html.split('aria-label="Hardware identity"')[1].split("</section>")[0];
  assert.deepEqual([...identity.matchAll(/<strong id="([^"]+)"/g)].map(match => match[1]),
    ["product-name", "board-model", "serial", "identity-clock", "ram-usage", "firmware-version"]);
  const runtime = html.split('<dl class="runtime-grid runtime-summary">')[1].split("</dl>")[0];
  assert.deepEqual([...runtime.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Uptime", "Reset cause", "Service load (Core 0 / Core 1)", "Board temperature", "CLI port", "Storage bus speed",
      "Wi-Fi", "Bluetooth"]);
  const storage = html.split('aria-labelledby="memory-title"')[1].split("</section>")[0];
  assert.deepEqual([...storage.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Memory state", "Capacity", "Filesystem", "Manufacturer", "Product", "Serial"]);
  assert.doesNotMatch(html, /id="memory-mode"/);
  assert.match(html, /<details class="pin-map-disclosure">/);
  assert.match(html, /id="board-uart"[\s\S]*id="board-storage-pins"/);
  for (const name of ["configuration", "reboot", "return", "share"]) {
    assert.match(html, new RegExp(`<dialog class="confirmation-dialog" id="${name}-dialog"`));
  }
  assert.match(html, /id="led-control"[\s\S]*?aria-pressed="false"/);
  assert.match(html, /<progress[^>]*aria-label="Benchmark progress"/);
  for (const name of ["telemetry", "storage", "self-test"]) {
    assert.match(html, new RegExp(`id="${name}-freshness" role="status"`));
  }
});

test("WebHID UI preserves unsaved profiles, separates confirmed reboot, and confirms force return", async context => {
  const saved = {
    document: globalThis.document, window: globalThis.window,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
  };
  const prototypes = new Map(["connect", "setMsc", "saveSettings", "reboot"].map(name =>
    [name, PicoForgeWebHid.prototype[name]]));
  const elements = new Map();
  const el = selector => {
    if (!elements.has(selector)) elements.set(selector, { value: "0", dataset: {}, valueAsNumber: 32,
      open: false, showModal() { this.open = true; }, close() { this.open = false; this.onclose?.(); },
      addEventListener(type, handler) { this[type === "close" ? "onclose" : type] = handler; },
      setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  let client;
  const commands = [];
  globalThis.document = { querySelector: el, createElement: () => ({ children: [],
    append(...children) { this.children.push(...children); } }) };
  globalThis.window = { confirm() { assert.fail("Use styled dialogs, not browser confirmations"); } };
  Object.defineProperty(globalThis, "navigator", { configurable: true, value: { hid: new EventTarget() } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  PicoForgeWebHid.prototype.connect = async function () {
    client = this;
    const hardware = decodeIdentityReport(identity());
    this.update({ connected: true, hardware, settings: decodeConfigReport(config(), hardware),
      settings_supported: true, freshness: { 3: "fresh", 10: "fresh", 11: "fresh" },
      msc: { enabled: false, transition_pending: false }, self_test: decodeSelfTestReport(selfTest()),
      memory: { sample_valid: true, initialized: true } });
  };
  PicoForgeWebHid.prototype.setMsc = async (...args) => { commands.push(["msc", ...args]); };
  PicoForgeWebHid.prototype.saveSettings = async function (settings, force = false) {
    commands.push(["save", settings]);
    this.testForceSave = force;
    this.update({ settings: { ...this.state.settings, ...settings } });
    return true;
  };
  PicoForgeWebHid.prototype.reboot = async confirmed => { commands.push(["reboot", confirmed]); };
  try {
    await import(`../js/app.mjs?test=${Date.now()}`);
    await el("#usb-open").click();
    await el("#usb-connect").click();
    assert.equal(el("#settings-storage").value, "1");
    assert.equal(el("#settings-note").hidden, true);
    assert.equal(el("#status-message").hidden, false);
    assert.equal(el("#settings-save").disabled, true);
    client.update({ self_test_command_pending: true });
    assert.equal(el("#settings-note").hidden, true);
    client.update({ self_test_command_pending: false, self_test: decodeSelfTestReport(selfTest(1)) });
    assert.equal(el("#settings-note").hidden, true);
    client.update({ self_test: decodeSelfTestReport(selfTest()) });
    client.update({ msc_command_pending: true });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
    client.update({ msc_command_pending: false });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, false);
    el("#settings-clock").value = "250"; el("#settings-clock").change();
    client.update({ error: null });
    assert.equal(el("#settings-clock").value, "250");
    await el("#settings-save").click();
    assert.deepEqual(commands, []);
    assert.equal(el("#configuration-dialog").open, true);
    assert.match(el("#configuration-dialog-profile").textContent, /250 MHz.*High-Speed/);
    el("#cancel-configuration").click();
    await el("#confirm-configuration").click();
    assert.deepEqual(commands, []);
    await el("#settings-save").click();
    await el("#confirm-configuration").click();
    assert.deepEqual(commands, [["save", { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 }]]);
    await el("#settings-reboot").click();
    assert.equal(commands.length, 1);
    assert.equal(el("#reboot-dialog").open, true);
    el("#reboot-dialog").keydown({ key: "Escape", preventDefault() {} });
    assert.equal(el("#reboot-dialog").open, false);
    await el("#confirm-reboot").click();
    assert.equal(commands.length, 1);
    await el("#settings-reboot").click();
    await el("#confirm-reboot").click();
    assert.deepEqual(commands[1], ["reboot", true]);
    await el("#msc-control").click();
    assert.equal(el("#share-dialog").open, true);
    assert.equal(commands.length, 2);
    el("#cancel-share").click();
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    await el("#msc-control").click();
    el("#share-dialog").keydown({ key: "Escape", preventDefault() {} });
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    await el("#msc-control").click();
    client.update({ self_test_command_pending: true });
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    client.update({ self_test_command_pending: false });
    await el("#msc-control").click();
    await el("#confirm-share").click();
    assert.deepEqual(commands[2], ["msc", true, false]);
    client.update({ msc: { enabled: true, transition_pending: false } });
    el("#settings-clock").value = "200"; el("#settings-clock").change();
    assert.equal(el("#settings-clock").disabled, false);
    assert.equal(el("#settings-save").disabled, true);
    client.update({ hardware: { ...client.state.hardware, capabilities: client.state.hardware.capabilities | 64 } });
    await el("#settings-save").click();
    assert.equal(el("#configuration-force-warning").hidden, false);
    el("#cancel-configuration").click();
    await el("#confirm-configuration").click();
    assert.equal(commands.length, 3);
    await el("#settings-save").click();
    await el("#confirm-configuration").click();
    assert.equal(client.testForceSave, true);
    commands.pop();
    await el("#msc-control").click();
    assert.equal(commands.length, 3);
    assert.equal(el("#return-dialog").open, true);
    el("#cancel-return").click();
    await el("#confirm-return").click();
    assert.equal(commands.length, 3);
    await el("#msc-control").click();
    await el("#confirm-return").click();
    assert.deepEqual(commands[3], ["msc", false, true]);
    await el("#msc-control").click();
    client.update({ outcome_uncertain: true });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    await el("#confirm-return").click();
    assert.equal(commands.length, 4);
    client.update({ outcome_uncertain: false });
    await el("#msc-control").click();
    assert.equal(el("#return-dialog").open, true);
    client.update({ connected: false });
    assert.equal(el("#return-dialog").open, false);
    await el("#confirm-return").click();
    assert.equal(commands.length, 4);
    client.update({ connected: true });
    client.update({ outcome_uncertain: false, freshness: { 3: "stale", 10: "stale", 11: "stale" },
      runtime: null, board_status: null, msc: null, self_test: null });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    assert.equal(el("#self-test-start").disabled, true);
    let now = Date.now();
    context.mock.method(Date, "now", () => now);
    let reads = 0;
    let wifiState = 2;
    let failure = false;
    let delayed = null;
    const device = new EventTarget();
    device.receiveFeatureReport = async id => {
      assert.equal(id, 30);
      reads++;
      if (failure) throw new Error("device read failed");
      if (delayed) return delayed;
      const report = new Uint8Array(48);
      report.set([1, 1, 0, 0, wifiState, 192, 168, 1, 9]);
      report.set(new TextEncoder().encode("HomeDev"), 9);
      return report;
    };
    client.device = device;
    client.update({ hardware: { ...client.state.hardware, capabilities: client.state.hardware.capabilities | 128 } });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#wifi-state").textContent, "HomeDev (192.168.1.9)");
    assert.equal(reads, 1);
    wifiState = 1;
    client.update({});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(reads, 1, "input reports must not issue unthrottled wireless reads");
    now += 2000;
    client.update({});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#wifi-state").textContent, "HomeDev (connecting)");
    failure = true;
    now += 2000;
    client.update({});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#wifi-state").textContent, "Unavailable");
    assert.match(el("#wifi-state").title, /device read failed/);
    failure = false;
    let finishRead;
    delayed = new Promise(resolve => { finishRead = resolve; });
    now += 2000;
    client.update({});
    client.clearConnection();
    finishRead(new Uint8Array(48));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(client.state.wireless, null, "a late report must not repopulate a disconnected board");
    assert.equal(el("#wifi-state").textContent, "--");
  } finally {
    for (const [name, method] of prototypes) PicoForgeWebHid.prototype[name] = method;
    globalThis.document = saved.document; globalThis.window = saved.window;
    if (saved.navigator) Object.defineProperty(globalThis, "navigator", saved.navigator);
    else delete globalThis.navigator;
    if (saved.secure) Object.defineProperty(globalThis, "isSecureContext", saved.secure);
    else delete globalThis.isSecureContext;
  }
});

test("connection dialogs scan before prompting for a key and preserve transport safety", async () => {
  const saved = {
    document: globalThis.document, navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
  };
  const prototypes = new Map(["connect", "disconnect", "setControlKey"].map(name =>
    [name, PicoForgeBluetooth.prototype[name]]));
  const elements = new Map();
  const el = selector => {
    if (!elements.has(selector)) elements.set(selector, { value: "", dataset: {}, valueAsNumber: 32, children: [],
      open: false, showModal() { this.open = true; },
      close() { this.open = false; this.events.close?.(); }, focus() { this.focused = true; }, events: {},
      addEventListener(type, handler) { this.events[type] = handler; if (type !== "close") this[type] = handler; },
      setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: el, createElement: () => ({ append() {} }) };
  Object.defineProperty(globalThis, "navigator", { configurable: true,
    value: { hid: new EventTarget(), bluetooth: {} } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  let client;
  let scans = 0;
  let failure = false;
  let finishScan;
  let loadedKey;
  let finishKey;
  let pauseKey = false;
  PicoForgeBluetooth.prototype.connect = async function () {
    client = this;
    scans++;
    this.update({ busy: true, error: null });
    await new Promise(resolve => { finishScan = resolve; });
    if (failure) {
      this.update({ busy: false, error: "No board selected." });
      return;
    }
    this.bleDevice = { name: "Nearby PicoForge" };
    this.update({ connected: true, busy: false, transport: "ble", hardware: decodeIdentityReport(identity()),
      board_status: { led_available: true, led_on: false }, freshness: { 10: "fresh", 11: "fresh" },
      msc: { enabled: false }, self_test: decodeSelfTestReport(selfTest()) });
  };
  PicoForgeBluetooth.prototype.setControlKey = async function (key) {
    if (!/^[a-f0-9]{64}$/i.test(key)) throw new Error("BLE key must be 64 nonzero hex digits.");
    if (pauseKey) await new Promise(resolve => { finishKey = resolve; });
    loadedKey = key;
    this.bleKey = {};
  };
  PicoForgeBluetooth.prototype.disconnect = async function () {
    this.bleKey = null;
    this.clearConnection();
  };
  try {
    await import(`../js/app.mjs?connections=${Date.now()}`);
    el("#usb-open").click();
    assert.equal(el("#usb-connection-dialog").open, true);
    el("#usb-connection-close").click();
    el("#bluetooth-open").click();
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(scans, 0, "opening a dialog must not scan without another user gesture");
    assert.equal(el("#bluetooth-key-step").hidden, true);
    assert.equal(el("#ble-authorize").disabled, true);
    failure = true;
    let scan = el("#bluetooth-scan").click();
    assert.equal(el("#bluetooth-scan").disabled, true);
    let prevented = false;
    el("#bluetooth-connection-dialog").cancel({ preventDefault() { prevented = true; } });
    assert.equal(prevented, true, "do not dismiss while a picker/connection is pending");
    finishScan(); await scan;
    assert.match(el("#bluetooth-connection-status").textContent, /No board selected/);
    assert.equal(el("#bluetooth-key-step").hidden, true);
    assert.equal(el("#bluetooth-scan").disabled, false);
    failure = false;
    scan = el("#bluetooth-scan").click();
    finishScan(); await scan;
    assert.equal(scans, 2);
    assert.equal(el("#bluetooth-key-step").hidden, false);
    assert.equal(el("#ble-control-key").focused, true);
    assert.match(el("#bluetooth-connection-status").textContent, /Nearby PicoForge/);
    assert.equal(el("#usb-open").disabled, true);
    assert.equal(el("#led-control").disabled, true);
    el("#self-test-size").input();
    assert.equal(el("#self-test-start").disabled, true);
    el("#ble-control-key").value = "bad";
    await el("#ble-authorize").click();
    assert.match(el("#bluetooth-connection-status").textContent, /64 nonzero hex digits/);
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(el("#ble-control-key").value, "");
    el("#ble-control-key").value = "ab".repeat(32);
    el("#bluetooth-connection-dialog").keydown({ key: "Escape", preventDefault() {} });
    assert.equal(el("#bluetooth-connection-dialog").open, false);
    assert.equal(el("#ble-control-key").value, "", "dismissal clears unsubmitted secrets");
    assert.equal(client.state.connected, true, "closing without a key retains read-only diagnostics");
    assert.equal(client.bleKey, null);
    el("#bluetooth-open").click();
    el("#ble-control-key").value = "ab".repeat(32);
    await el("#ble-authorize").click();
    assert.equal(loadedKey, "ab".repeat(32));
    assert.equal(el("#bluetooth-connection-dialog").open, false);
    assert.equal(el("#ble-control-key").value, "");
    assert.equal(el("#led-control").disabled, false);
    client.outstanding = { opcode: 1, transaction_id: 1 };
    client.update({ outcome_uncertain: true });
    assert.equal(el("#usb-open").disabled, true);
    el("#bluetooth-open").click();
    await el("#bluetooth-disconnect").click();
    assert.equal(client.bleKey, null);
    assert.equal(el("#bluetooth-key-step").hidden, true);
    assert.equal(el("#usb-open").disabled, true, "disconnect must not discard a pending command to switch transport");
    client.outstanding = null;
    client.update({ outcome_uncertain: false });
    assert.equal(el("#usb-open").disabled, false);
    scan = el("#bluetooth-scan").click();
    finishScan(); await scan;
    pauseKey = true;
    el("#ble-control-key").value = "ab".repeat(32);
    const authorization = el("#ble-authorize").click();
    assert.equal(el("#bluetooth-connection-close").disabled, true);
    await client.disconnect();
    finishKey(); await authorization;
    assert.equal(client.bleKey, null, "a key imported after disconnect must not survive");
    assert.match(el("#bluetooth-connection-status").textContent, /disconnected while loading/);
    Object.defineProperty(globalThis, "navigator", { configurable: true, value: {} });
    el("#bluetooth-open").click();
    client.update({});
    assert.equal(el("#bluetooth-scan").disabled, true);
    assert.match(el("#bluetooth-connection-status").textContent, /supported browser/);
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: false });
    client.update({});
    assert.equal(el("#usb-connect").disabled, true);
  } finally {
    for (const [name, method] of prototypes) PicoForgeBluetooth.prototype[name] = method;
    globalThis.document = saved.document;
    for (const [name, descriptor] of [["navigator", saved.navigator], ["isSecureContext", saved.secure]]) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    }
  }
});

test("server dashboard imports only the WebHID module the Python and .NET servers publish", () => {
  for (const name of ["app.mjs", "dashboard.mjs"]) {
    const source = readFileSync(new URL(`../../web/js/${name}`, import.meta.url), "utf8");
    for (const [, path] of source.matchAll(/from "\.\.\/\.\.\/webhid\/js\/([^"?]+)/g)) {
      assert.equal(path, "wireless-form.mjs", `${name} imports unpublished ${path}`);
    }
  }
  const form = readFileSync(new URL("../js/wireless-form.mjs", import.meta.url), "utf8");
  assert.doesNotMatch(form, /^import /m);
});
