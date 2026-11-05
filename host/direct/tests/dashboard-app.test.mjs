import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import test from "node:test";
import { PicoForgeWebHid } from "../js/device.mjs?v=12";
import { PicoForgeBluetooth } from "../js/bluetooth.mjs?v=15";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../js/device.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

const memorySessionStorage = () => {
  const saved = new Map();
  return { getItem: key => saved.get(key) ?? null, setItem: (key, value) => saved.set(key, value),
    removeItem: key => saved.delete(key) };
};

test("Direct UI preserves unsaved profiles, separates confirmed reboot, and confirms force return", async context => {
  const saved = {
    document: globalThis.document, window: globalThis.window,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
    sessionStorage: Object.getOwnPropertyDescriptor(globalThis, "sessionStorage"),
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
  Object.defineProperty(globalThis, "sessionStorage", { configurable: true, value: memorySessionStorage() });
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
    client.update({ recovery_message: "Pending command result confirmed after reconnect." });
    assert.equal(el("#status-message").textContent, "Pending command result confirmed after reconnect.");
    client.update({ recovery_message: null });
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
    client.now = () => now;
    context.mock.method(Date, "now", () => now);
    let reads = 0;
    let wifiState = 2;
    let failure = false;
    let delayed = null;
    const device = new EventTarget();
    device.receiveFeatureReport = async id => {
      assert.equal(id, 12);
      reads++;
      if (failure) throw new Error("device read failed");
      if (delayed) return delayed;
      const report = new Uint8Array(48);
      report.set([1, wifiState, 0, 0, 1]);
      if (wifiState === 2) report.set([192, 168, 1, 9], 5);
      report[9] = 7;
      report.set(new TextEncoder().encode("HomeDev"), 10);
      return report;
    };
    client.device = device;
    client.update({ settings: { ...client.state.settings, radios: 1 },
      hardware: { ...client.state.hardware, capabilities: client.state.hardware.capabilities | 128 } });
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
    assert.equal(el("#wifi-state").textContent, "HomeDev (connecting)", "one failed poll retains a fresh snapshot");
    now += 4000;
    client.expireReports();
    assert.equal(el("#wifi-state").textContent, "Unavailable");
    assert.match(el("#wifi-state").title, /stale/);
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
    if (saved.sessionStorage) Object.defineProperty(globalThis, "sessionStorage", saved.sessionStorage);
    else delete globalThis.sessionStorage;
  }
});

test("Bluetooth dialog takes the control key, then scans, and preserves transport safety", async () => {
  const saved = {
    document: globalThis.document, window: globalThis.window,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
    sessionStorage: Object.getOwnPropertyDescriptor(globalThis, "sessionStorage"),
    crypto: Object.getOwnPropertyDescriptor(globalThis, "crypto"),
  };
  const prototypes = new Map(["connect", "disconnect", "tcpCliAuthProof", "saveSettings", "reboot"].map(name =>
    [name, PicoForgeBluetooth.prototype[name]]));
  const elements = new Map();
  const authChannels = [];
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
  class FakeBroadcastChannel {
    constructor(name) { this.name = name; this.messages = []; authChannels.push(this); }
    postMessage(message) { this.messages.push(message); }
    close() { this.closed = true; }
  }
  globalThis.window = { BroadcastChannel: FakeBroadcastChannel };
  Object.defineProperty(globalThis, "navigator", { configurable: true,
    value: { hid: new EventTarget(), bluetooth: {} } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  Object.defineProperty(globalThis, "sessionStorage", { configurable: true, value: memorySessionStorage() });
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: webcrypto });
  let client;
  let scans = 0;
  let failure = false;
  let finishScan;
  let usedKey;
  const authCalls = [];
  const settingsCommands = [];
  PicoForgeBluetooth.prototype.saveSettings = async function (settings, force, tcpCliEnabled) {
    settingsCommands.push(["save", settings, force, tcpCliEnabled]);
    this.update({ settings: { ...this.state.settings, ...settings,
      ...(tcpCliEnabled === undefined ? {} : { tcp_cli_enabled: tcpCliEnabled }) },
    reboot_pending: true });
    return true;
  };
  PicoForgeBluetooth.prototype.reboot = async confirmed => { settingsCommands.push(["reboot", confirmed]); };
  PicoForgeBluetooth.prototype.connect = async function (key) {
    client = this;
    scans++;
    usedKey = key;
    this.update({ busy: true, error: null });
    await new Promise(resolve => { finishScan = resolve; });
    if (failure) {
      this.update({ busy: false, error: "The board rejected the control key." });
      return;
    }
    this.bleKey = {};
    this.bleDevice = { name: "Nearby PicoForge" };
    const hardware = decodeIdentityReport(identity(128 | 55));
    const wirelessReport = new Uint8Array(53);
    wirelessReport[0] = 1;
    this.device = { async receiveFeatureReport() { return wirelessReport; }, removeEventListener() {} };
    this.update({ connected: true, busy: false, transport: "ble", hardware,
      settings: decodeConfigReport(config(), hardware), settings_supported: true,
      board_status: { led_available: true, led_on: false },
      freshness: { 3: "fresh", 10: "fresh", 11: "fresh" },
      msc: { enabled: false, transition_pending: false }, self_test: decodeSelfTestReport(selfTest()),
      memory: { sample_valid: true, initialized: true },
      wireless: { ssid: "HomeDev", wifi_enabled: true, ble_enabled: true, password_configured: true,
        ble_key_configured: true, tcp_cli_enabled: true } });
  };
  PicoForgeBluetooth.prototype.disconnect = async function () {
    this.bleKey = null;
    this.clearConnection();
  };
  PicoForgeBluetooth.prototype.tcpCliAuthProof = async function (nonce, uid) {
    authCalls.push([Array.from(nonce), Array.from(uid)]);
    return Uint8Array.from({ length: 16 }, (_, index) => index);
  };
  try {
    await import(`../js/app.mjs?connections=${Date.now()}`);
    el("#usb-open").click();
    assert.equal(el("#usb-connection-dialog").open, true);
    el("#usb-connection-close").click();
    el("#bluetooth-open").click();
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(scans, 0, "opening a dialog must not scan without another user gesture");
    assert.match(el("#bluetooth-connection-status").textContent, /Enter the control key/);
    assert.equal(el("#ble-control-key").disabled, false);
    failure = true;
    el("#ble-control-key").value = "cd".repeat(32);
    let scan = el("#bluetooth-scan").click();
    assert.equal(usedKey, "cd".repeat(32));
    assert.equal(el("#ble-control-key").value, "", "the key input is cleared as soon as it is submitted");
    assert.equal(el("#bluetooth-scan").disabled, true);
    assert.equal(el("#ble-control-key").disabled, true);
    let prevented = false;
    el("#bluetooth-connection-dialog").cancel({ preventDefault() { prevented = true; } });
    assert.equal(prevented, true, "do not dismiss while a picker/connection is pending");
    finishScan(); await scan;
    assert.match(el("#bluetooth-connection-status").textContent, /rejected the control key/);
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(el("#bluetooth-scan").disabled, false);
    failure = false;
    el("#ble-control-key").value = "ab".repeat(32);
    scan = el("#bluetooth-scan").click();
    finishScan(); await scan;
    assert.equal(scans, 2);
    assert.equal(usedKey, "ab".repeat(32));
    assert.equal(el("#bluetooth-connection-dialog").open, false, "a successful unlock closes the dialog");
    assert.equal(el("#usb-open").disabled, true);
    assert.equal(el("#led-control").disabled, false);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#settings-clock").disabled, false, "BLE enables clock configuration");
    assert.equal(el("#settings-storage").disabled, false, "BLE enables SD profile configuration");
    assert.equal(el("#settings-wifi").disabled, true, "radio assignments stay USB-only");
    assert.equal(el("#settings-ble").disabled, true, "radio assignments stay USB-only");
    assert.equal(el("#settings-tcp-cli").disabled, false, "BLE can toggle TCP CLI without Wi-Fi provisioning");
    assert.equal(el("#wifi-scan").disabled, true, "Wi-Fi provisioning actions stay USB-only");
    assert.equal(el("#ble-generate").disabled, true, "Control-key provisioning stays USB-only");
    assert.equal(el("#msc-control").disabled, true, "storage ownership stays USB-only");
    await el("#settings-reboot").click();
    await el("#confirm-reboot").click();
    assert.deepEqual(settingsCommands, [["reboot", true]]);
    el("#settings-clock").value = "250";
    el("#settings-clock").change();
    el("#settings-tcp-cli").value = "0";
    el("#settings-tcp-cli").change();
    await el("#settings-save").click();
    assert.match(el("#configuration-dialog-profile").textContent, /TCP CLI: Disabled/);
    assert.match(el("#configuration-wireless-warning").textContent, /without sending Wi-Fi credentials/);
    await el("#confirm-configuration").click();
    assert.deepEqual(settingsCommands[1], ["save",
      { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 }, false, false]);
    client.update({ reboot_pending: false });
    const wireless = { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
      tcp_cli_auth_required: true, ssid: "HomeDev", ipv4: "192.168.66.87" };
    client.update({ wireless });
    const consoleLink = el("#wifi-state").children[0];
    const consoleUrl = new URL(consoleLink.href, "http://localhost");
    assert.equal(consoleUrl.searchParams.get("ipv4"), "192.168.66.87");
    assert.equal(consoleUrl.searchParams.get("auth"), "1");
    const authToken = consoleUrl.searchParams.get("auth_token");
    assert.match(authToken, /^[a-f0-9]{32}$/i);
    const authRequest = { type: "tcp-cli-auth-request", auth_token: authToken,
      request_id: "00112233445566778899aabbccddeeff",
      nonce: "000102030405060708090a0b0c0d0e0f", uid: "0102030405060708" };
    authChannels[0].onmessage({ data: authRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(authCalls, [[Array.from({ length: 16 }, (_, index) => index),
      Array.from({ length: 8 }, (_, index) => index + 1)]]);
    assert.deepEqual(authChannels[0].messages[0], { type: "tcp-cli-auth-response", auth_token: authToken,
      request_id: authRequest.request_id, uid: authRequest.uid, proof: "000102030405060708090a0b0c0d0e0f" });
    authChannels[0].onmessage({ data: { ...authRequest, uid: "0807060504030201" } });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(authCalls.length, 1, "the connected dashboard refuses a different board identity");
    assert.equal(authChannels[0].messages[1].type, "tcp-cli-auth-error");
    assert.match(authChannels[0].messages[1].error, /different board/);
    client.outstanding = { opcode: 1, transaction_id: 1 };
    client.update({ outcome_uncertain: true });
    assert.equal(el("#usb-open").disabled, true);
    el("#bluetooth-open").click();
    assert.match(el("#bluetooth-connection-status").textContent, /Connected to Nearby PicoForge/);
    assert.equal(el("#ble-control-key").disabled, true);
    assert.equal(el("#bluetooth-scan").textContent, "Disconnect", "one button toggles while connected");
    assert.equal(el("#bluetooth-scan").disabled, false);
    await el("#bluetooth-scan").click();
    assert.equal(client.bleKey, null);
    assert.equal(el("#usb-open").disabled, true, "disconnect must not discard a pending command to switch transport");
    client.outstanding = null;
    client.update({ outcome_uncertain: false });
    assert.equal(el("#usb-open").disabled, false);
    el("#ble-control-key").value = "ab".repeat(32);
    el("#bluetooth-connection-close").click();
    assert.equal(el("#ble-control-key").value, "", "closing the dialog discards an unsubmitted key");
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
    globalThis.window = saved.window;
    for (const [name, descriptor] of [["navigator", saved.navigator], ["isSecureContext", saved.secure],
      ["sessionStorage", saved.sessionStorage], ["crypto", saved.crypto]]) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    }
  }
});
