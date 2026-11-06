// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { webcrypto } from "node:crypto";
import { BoardSession } from "../../web/js/board/session.mjs";
import { BleTransport } from "../../web/js/transport/ble.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../../web/js/protocol/codec.mjs";
import { identity, config, selfTest, patchState } from "../support/fixtures.mjs";
import { fakeConnection, storage, emit } from "../support/fixtures.mjs";
import { createDocument } from "../support/dom.mjs";
import { mountDashboard } from "../../web/js/ui/dashboard.mjs";
import { createTimerScope } from "../../web/js/timers.mjs";
import { OP } from "../../web/js/protocol/constants.mjs";

test("save completions own their original draft, including verified forced-save recovery", async () => {
  const previous = { document: globalThis.document, window: globalThis.window };
  globalThis.window = { addEventListener() {}, removeEventListener() {} };
  try {
    for (const outcome of ["resolve", "reject", "forced", "forced-new-draft", "dispose"]) {
      const { document, element: el, fire } = createDocument();
      globalThis.document = document;
      const { client, device } = fakeConnection({ capabilities: 128 | 55 });
      const ready = async () => {
        await client.connect();
        emit(device, 10, storage());
        emit(device, 11, selfTest());
        await client.readWireless();
        await null;
      };
      await ready();
      let resolveSave, rejectSave;
      client.saveConfiguration = () => new Promise((resolve, reject) => {
        resolveSave = resolve;
        rejectSave = reject;
      });
      const timers = createTimerScope();
      const dashboard = mountDashboard({ session: client, selectTransport: () => client, timers,
        environment: { secure: true, hid: true, ble: false, now: Date.now, clipboard: null } });
      try {
        const forced = outcome.startsWith("forced");
        if (forced) patchState(client, { msc: { enabled: true, transition_pending: false } });
        await null;
        el("#settings-clock").value = "250";
        await fire("#settings-clock", "change");
        await null;
        await fire("#settings-save", "click");
        const saving = fire("#confirm-configuration", "click");
        assert.equal(typeof resolveSave, "function", outcome);
        await client.disconnect();
        await null;
        if (outcome === "dispose") dashboard.dispose();
        else await ready();
        if (forced) patchState(client, { recovered_opcode: OP.SAVE_SETTINGS });
        if (outcome !== "forced" && outcome !== "dispose") await fire("#direct-ble-generate", "click");
        await null;
        const key = el("#direct-ble-secret").value;
        if (outcome === "reject" || outcome === "dispose") rejectSave(new Error("Old save failed"));
        else resolveSave({ tcp_cli_enabled: true });
        await saving;
        await null;
        assert.equal(el("#direct-ble-secret").value, key, `${outcome}: newer key is preserved`);
        if (outcome === "forced") {
          assert.match(el("#settings-note").textContent, /Settings saved/);
        } else {
          assert.doesNotMatch(el("#settings-note").textContent, /Settings saved|Old save failed/, outcome);
        }
      } finally { dashboard.dispose(); client.dispose(); timers.dispose(); }
    }
  } finally { globalThis.document = previous.document; globalThis.window = previous.window; }
});

const memorySessionStorage = () => {
  const saved = new Map();
  return { getItem: key => saved.get(key) ?? null, setItem: (key, value) => saved.set(key, value),
    removeItem: key => saved.delete(key) };
};

test("Direct UI preserves unsaved profiles, separates confirmed reboot, and confirms force return", async context => {
  const saved = {
    document: globalThis.document, window: globalThis.window,
    location: globalThis.location,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
    sessionStorage: Object.getOwnPropertyDescriptor(globalThis, "sessionStorage"),
  };
  const prototypes = new Map(["connect", "setMsc", "saveSettings", "reboot"].map(name =>
    [name, BoardSession.prototype[name]]));
  const { document, element: el, fire } = createDocument();
  let client;
  /** Apply a client change and flush the dashboard's coalesced refresh. */
  const sync = async changes => { patchState(client, changes); await null; };
  const commands = [];
  globalThis.document = document;
  globalThis.window = { confirm() { assert.fail("Use styled dialogs, not browser confirmations"); } };
  Object.defineProperty(globalThis, "navigator", { configurable: true, value: { hid: new EventTarget() } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  Object.defineProperty(globalThis, "sessionStorage", { configurable: true, value: memorySessionStorage() });
  BoardSession.prototype.connect = async function () {
    client = this;
    const hardware = decodeIdentityReport(identity());
    patchState(this, { connected: true, hardware, settings: decodeConfigReport(config(), hardware),
      settings_supported: true, freshness: { 3: "fresh", 10: "fresh", 11: "fresh" },
      msc: { enabled: false, transition_pending: false }, self_test: decodeSelfTestReport(selfTest()),
      memory: { sample_valid: true, initialized: true } });
  };
  BoardSession.prototype.setMsc = async (...args) => { commands.push(["msc", ...args]); };
  BoardSession.prototype.saveSettings = async function (settings, force = false) {
    commands.push(["save", settings]);
    this.testForceSave = force;
    patchState(this, { settings: { ...this.state.settings, ...settings } });
    return true;
  };
  BoardSession.prototype.reboot = async confirmed => { commands.push(["reboot", confirmed]); };
  try {
    await import(`../../web/js/app.mjs?test=${Date.now()}`);
    await fire("#usb-open", "click");
    await fire("#usb-connect", "click");
    assert.equal(el("#settings-storage").value, "1");
    assert.equal(el("#settings-note").hidden, true);
    assert.equal(el("#status-message").hidden, false);
    const label = el("#connection-label");
    let labelText = label.textContent;
    let renders = 0;
    Object.defineProperty(label, "textContent", { configurable: true, get: () => labelText,
      set: value => { renders++; labelText = value; } });
    for (const error of ["first", "second", null]) patchState(client, { error });
    assert.equal(renders, 0, "client changes render asynchronously");
    await null;
    assert.equal(renders, 0, "a coalesced refresh does not rewrite an unchanged connection label");
    await sync({ recovery_message: "Pending command result confirmed after reconnect." });
    assert.equal(el("#status-message").textContent, "Pending command result confirmed after reconnect.");
    await sync({ recovery_message: null });
    assert.equal(el("#settings-save").disabled, true);
    await sync({ pending_self_test: { sequence: client.state.self_test?.sequence ?? null } });
    assert.equal(el("#settings-note").hidden, true);
    await sync({ pending_self_test: null, self_test: decodeSelfTestReport(selfTest(1)) });
    assert.equal(el("#settings-note").hidden, true);
    await sync({ self_test: decodeSelfTestReport(selfTest()) });
    await sync({ pending_msc: true });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
    await sync({ pending_msc: null });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, false);
    el("#settings-clock").value = "250"; await fire("#settings-clock", "change"); await null;
    await sync({ error: null });
    assert.equal(el("#settings-clock").value, "250");
    await fire("#settings-save", "click");
    assert.deepEqual(commands, []);
    assert.equal(el("#configuration-dialog").open, true);
    assert.match(el("#configuration-dialog-profile").textContent, /250 MHz.*High-Speed/);
    await fire("#cancel-configuration", "click");
    await fire("#confirm-configuration", "click");
    assert.deepEqual(commands, []);
    await fire("#settings-save", "click");
    await fire("#confirm-configuration", "click");
    assert.deepEqual(commands, [["save", { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 }]]);
    await fire("#settings-reboot", "click");
    assert.equal(commands.length, 1);
    assert.equal(el("#reboot-dialog").open, true);
    const escape = Object.assign(new Event("keydown", { cancelable: true }), { key: "Escape" });
    el("#reboot-dialog").dispatchEvent(escape);
    assert.equal(el("#reboot-dialog").open, false);
    await fire("#confirm-reboot", "click");
    assert.equal(commands.length, 1);
    await fire("#settings-reboot", "click");
    await fire("#confirm-reboot", "click");
    assert.deepEqual(commands[1], ["reboot", true]);
    await fire("#msc-control", "click");
    assert.equal(el("#share-dialog").open, true);
    assert.equal(commands.length, 2);
    await fire("#cancel-share", "click");
    await fire("#confirm-share", "click");
    assert.equal(commands.length, 2);
    await fire("#msc-control", "click");
    el("#share-dialog").dispatchEvent(escape);
    await fire("#confirm-share", "click");
    assert.equal(commands.length, 2);
    await fire("#msc-control", "click");
    await sync({ pending_self_test: { sequence: client.state.self_test?.sequence ?? null } });
    await fire("#confirm-share", "click");
    assert.equal(commands.length, 2);
    await sync({ pending_self_test: null });
    await fire("#msc-control", "click");
    await fire("#confirm-share", "click");
    assert.deepEqual(commands[2], ["msc", true, false]);
    await sync({ msc: { enabled: true, transition_pending: false } });
    el("#settings-clock").value = "200"; await fire("#settings-clock", "change"); await null;
    assert.equal(el("#settings-clock").disabled, false);
    await fire("#settings-save", "click");
    assert.equal(el("#configuration-force-warning").hidden, false);
    await fire("#cancel-configuration", "click");
    await fire("#confirm-configuration", "click");
    assert.equal(commands.length, 3);
    await fire("#settings-save", "click");
    await fire("#confirm-configuration", "click");
    assert.equal(client.testForceSave, true);
    commands.pop();
    await fire("#msc-control", "click");
    assert.equal(commands.length, 3);
    assert.equal(el("#return-dialog").open, true);
    await fire("#cancel-return", "click");
    await fire("#confirm-return", "click");
    assert.equal(commands.length, 3);
    await fire("#msc-control", "click");
    await fire("#confirm-return", "click");
    assert.deepEqual(commands[3], ["msc", false, true]);
    await fire("#msc-control", "click");
    await sync({ outcome_uncertain: true });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    await fire("#confirm-return", "click");
    assert.equal(commands.length, 4);
    await sync({ outcome_uncertain: false });
    await fire("#msc-control", "click");
    assert.equal(el("#return-dialog").open, true);
    await sync({ connected: false });
    assert.equal(el("#return-dialog").open, false);
    await fire("#confirm-return", "click");
    assert.equal(commands.length, 4);
    await sync({ connected: true });
    await sync({ outcome_uncertain: false, freshness: { 3: "stale", 10: "stale", 11: "stale" },
      runtime: null, msc: null, self_test: null });
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
    client.transport.device = device;
    await sync({ settings: { ...client.state.settings, radios: 1 },
      hardware: { ...client.state.hardware, capabilities: client.state.hardware.capabilities | 128 } });
    client.polling.enabled = true;
    client.pollWireless();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#direct-wifi-state").textContent, "HomeDev (192.168.1.9)");
    assert.equal(reads, 1);
    wifiState = 1;
    client.pollWireless();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(reads, 1, "polls inside the interval must not read Feature 12");
    now += 2000;
    client.pollWireless();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#direct-wifi-state").textContent, "HomeDev (connecting)");
    failure = true;
    now += 2000;
    client.pollWireless();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#direct-wifi-state").textContent, "HomeDev (connecting)", "one failed poll retains a fresh snapshot");
    now += 4000;
    client.expireReports(); await null;
    assert.equal(el("#direct-wifi-state").textContent, "Unavailable");
    assert.match(el("#direct-wifi-state").title, /stale/);
    failure = false;
    let finishRead;
    delayed = new Promise(resolve => { finishRead = resolve; });
    now += 2000;
    client.pollWireless();
    client.connection.clear();
    finishRead(new Uint8Array(48));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(client.state.wireless, null, "a late report must not repopulate a disconnected board");
    assert.equal(el("#direct-wifi-state").textContent, "--");
  } finally {
    for (const [name, method] of prototypes) BoardSession.prototype[name] = method;
    globalThis.document = saved.document; globalThis.window = saved.window;
    if (saved.location === undefined) delete globalThis.location;
    else globalThis.location = saved.location;
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
    location: globalThis.location,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
    sessionStorage: Object.getOwnPropertyDescriptor(globalThis, "sessionStorage"),
    crypto: Object.getOwnPropertyDescriptor(globalThis, "crypto"),
  };
  const prototypes = new Map(["connect", "disconnect", "saveSettings", "reboot"].map(name =>
    [name, BoardSession.prototype[name]]));
  const createConsoleProof = BleTransport.prototype.createConsoleProof;
  const { document, element: el, fire } = createDocument();
  const windowHandlers = new Map();
  let reloads = 0;
  let consolePopup;
  globalThis.document = document;
  globalThis.location = { origin: "http://localhost", href: "http://localhost/", reload() { reloads++; } };
  globalThis.window = {
    addEventListener(type, handler) { windowHandlers.set(type, handler); },
    open(url) {
      consolePopup = { url, responses: [], postMessage(message, origin) { this.responses.push([message, origin]); } };
      return consolePopup;
    },
  };
  Object.defineProperty(globalThis, "navigator", { configurable: true,
    value: { hid: new EventTarget(), bluetooth: {} } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  Object.defineProperty(globalThis, "sessionStorage", { configurable: true, value: memorySessionStorage() });
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: webcrypto });
  let client;
  const sync = async changes => { patchState(client, changes); await null; };
  let scans = 0;
  let failure = false;
  let finishScan;
  let usedKey;
  const authCalls = [];
  const settingsCommands = [];
  BoardSession.prototype.saveSettings = async function (settings, force, tcpCliEnabled) {
    settingsCommands.push(["save", settings, force, tcpCliEnabled]);
    patchState(this, { settings: { ...this.state.settings, ...settings,
      ...(tcpCliEnabled === undefined ? {} : { tcp_cli_enabled: tcpCliEnabled }) },
    reboot_pending: true });
    return true;
  };
  BoardSession.prototype.reboot = async confirmed => { settingsCommands.push(["reboot", confirmed]); };
  BoardSession.prototype.connect = async function (key) {
    client = this;
    scans++;
    usedKey = Buffer.from(key).toString("hex");
    patchState(this, { busy: true, error: null });
    await new Promise(resolve => { finishScan = resolve; });
    if (failure) {
      patchState(this, { busy: false, error: "The board rejected the control key." });
      return;
    }
    this.transport.bleKey = {};
    this.transport.device = Object.assign(new EventTarget(), {
      name: "Nearby PicoForge", gatt: { connected: true, disconnect() {} },
    });
    const hardware = decodeIdentityReport(identity(128 | 55));
    const wirelessReport = new Uint8Array(53);
    wirelessReport[0] = 1;
    this.transport.readReport = async () => wirelessReport;
    patchState(this, { connected: true, busy: false, hardware,
      settings: decodeConfigReport(config(), hardware), settings_supported: true,
      runtime: { led_available: true, led_on: false },
      freshness: { 3: "fresh", 10: "fresh", 11: "fresh" },
      msc: { enabled: false, transition_pending: false }, self_test: decodeSelfTestReport(selfTest()),
      memory: { sample_valid: true, initialized: true },
      wireless: { ssid: "HomeDev", wifi_enabled: true, ble_enabled: true,
        ble_key_configured: true, tcp_cli_enabled: true } });
  };
  BoardSession.prototype.disconnect = async function () {
    this.transport.bleKey = null;
    this.transport.device = null;
    this.connection.clear();
  };
  BleTransport.prototype.createConsoleProof = async function (nonce, uid) {
    authCalls.push([Array.from(nonce), Array.from(uid)]);
    return Uint8Array.from({ length: 16 }, (_, index) => index);
  };
  try {
    await import(`../../web/js/app.mjs?connections=${Date.now()}`);
    windowHandlers.get("pageshow")({ persisted: false });
    assert.equal(reloads, 0, "initial page display must not reload");
    await fire("#usb-open", "click"); await null;
    assert.equal(el("#usb-connection-dialog").open, true);
    await fire("#usb-connection-close", "click");
    await fire("#bluetooth-open", "click"); await null;
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(scans, 0, "opening a dialog must not scan without another user gesture");
    assert.match(el("#bluetooth-connection-status").textContent, /Enter the control key/);
    assert.equal(el("#direct-ble-control-key").disabled, false);
    el("#direct-ble-control-key").value = "not-a-key";
    await fire("#bluetooth-scan", "click");
    await null;
    assert.equal(scans, 0, "invalid input must not open the device chooser");
    assert.match(el("#bluetooth-connection-status").textContent, /Invalid Control key/);
    assert.match(el("#status-message").textContent, /Invalid Control key/);
    await fire("#bluetooth-open", "click"); await null;
    assert.match(el("#bluetooth-connection-status").textContent, /Invalid Control key/,
      "another disconnected render preserves the error");
    failure = true;
    el("#direct-ble-control-key").value = "cd".repeat(32);
    let scan = fire("#bluetooth-scan", "click");
    assert.equal(usedKey, "cd".repeat(32));
    assert.equal(el("#direct-ble-control-key").value, "", "the key input is cleared as soon as it is submitted");
    await null;
    assert.equal(el("#bluetooth-scan").disabled, true);
    assert.equal(el("#direct-ble-control-key").disabled, true);
    const cancel = new Event("cancel", { cancelable: true });
    el("#bluetooth-connection-dialog").dispatchEvent(cancel);
    assert.equal(cancel.defaultPrevented, true, "do not dismiss while a picker/connection is pending");
    finishScan(); await scan;
    assert.match(el("#bluetooth-connection-status").textContent, /rejected the control key/);
    assert.equal(el("#bluetooth-connection-dialog").open, true);
    assert.equal(el("#bluetooth-scan").disabled, false);
    failure = false;
    el("#direct-ble-control-key").value = "ab".repeat(32);
    scan = fire("#bluetooth-scan", "click");
    finishScan(); await scan;
    assert.equal(scans, 2);
    assert.equal(usedKey, "ab".repeat(32));
    assert.equal(el("#bluetooth-connection-dialog").open, false, "a successful unlock closes the dialog");
    assert.equal(el("#usb-open").disabled, true);
    assert.equal(el("#led-control").disabled, false);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(el("#settings-clock").disabled, false, "BLE enables clock configuration");
    assert.equal(el("#settings-storage").disabled, false, "BLE enables SD profile configuration");
    assert.equal(el("#direct-settings-wifi").disabled, true, "radio assignments stay USB-only");
    assert.equal(el("#direct-settings-ble").disabled, true, "radio assignments stay USB-only");
    assert.equal(el("#direct-settings-tcp-cli").disabled, false, "BLE can toggle TCP CLI without Wi-Fi provisioning");
    assert.equal(el("#direct-wifi-scan").disabled, true, "Wi-Fi provisioning actions stay USB-only");
    assert.equal(el("#direct-ble-generate").disabled, true, "Control-key provisioning stays USB-only");
    assert.equal(el("#msc-control").disabled, true, "storage ownership stays USB-only");
    await fire("#settings-reboot", "click");
    await fire("#confirm-reboot", "click");
    assert.deepEqual(settingsCommands, [["reboot", true]]);
    el("#settings-clock").value = "250";
    await fire("#settings-clock", "change"); await null;
    el("#direct-settings-tcp-cli").value = "0";
    await fire("#direct-settings-tcp-cli", "change"); await null;
    await fire("#settings-save", "click");
    assert.match(el("#configuration-dialog-profile").textContent, /TCP CLI: Disabled/);
    assert.match(el("#direct-configuration-wireless-warning").textContent, /without sending Wi-Fi credentials/);
    await fire("#confirm-configuration", "click");
    assert.deepEqual(settingsCommands[1], ["save",
      { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 }, false, false]);
    await sync({ reboot_pending: false });
    const wireless = { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
      ssid: "HomeDev", ipv4: "192.168.66.87" };
    await sync({ wireless });
    const consoleLink = el("#direct-wifi-state").children[0];
    const consoleUrl = new URL(consoleLink.href, "http://localhost");
    assert.equal(consoleUrl.searchParams.get("ipv4"), "192.168.66.87");
    assert.equal(consoleUrl.searchParams.has("auth_token"), false, "the pairing secret is not in the URL");
    let consoleClickPrevented = false;
    consoleLink.onclick({ preventDefault() { consoleClickPrevented = true; } });
    assert.equal(consoleClickPrevented, true);
    assert.equal(consolePopup.url, consoleUrl.href);
    const authRequest = { type: "direct-console-auth-request",
      request_id: "00112233445566778899aabbccddeeff",
      nonce: "000102030405060708090a0b0c0d0e0f", uid: "0102030405060708" };
    windowHandlers.get("message")({ origin: "http://localhost", source: {}, data: authRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(authCalls, [], "unregistered same-origin windows cannot obtain a proof");
    windowHandlers.get("message")({ origin: "http://localhost", source: consolePopup, data: authRequest });
    windowHandlers.get("message")({ origin: "http://localhost", source: consolePopup, data: authRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.deepEqual(authCalls, [[Array.from({ length: 16 }, (_, index) => index),
      Array.from({ length: 8 }, (_, index) => index + 1)]], "only one proof request per console may run at a time");
    assert.deepEqual(consolePopup.responses[0], [{ type: "direct-console-auth-response",
      request_id: authRequest.request_id, uid: authRequest.uid,
      proof: "000102030405060708090a0b0c0d0e0f" }, "http://localhost"]);
    windowHandlers.get("message")({ origin: "https://example.com", source: consolePopup, data: authRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(authCalls.length, 1, "cross-origin requests remain unauthorized");
    const reconnectRequest = { ...authRequest, request_id: "11".repeat(16), nonce: "22".repeat(16) };
    windowHandlers.get("message")({ origin: "http://localhost", source: consolePopup, data: reconnectRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(authCalls.length, 2, "the same console can request a new proof after disconnecting");
    assert.equal(consolePopup.responses[1][0].request_id, reconnectRequest.request_id);
    consoleLink.onclick({ preventDefault() {} });
    const wrongIdentityRequest = { ...authRequest, request_id: "ffeeddccbbaa99887766554433221100",
      uid: "0807060504030201" };
    windowHandlers.get("message")({ origin: "http://localhost", source: consolePopup, data: wrongIdentityRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(authCalls.length, 2, "the connected dashboard refuses a different board identity");
    assert.equal(consolePopup.responses[0][0].type, "direct-console-auth-error");
    assert.match(consolePopup.responses[0][0].error, /different board/);
    await sync({ hardware: { ...client.state.hardware, serial_number: wrongIdentityRequest.uid.toUpperCase() } });
    windowHandlers.get("message")({ origin: "http://localhost", source: consolePopup, data: wrongIdentityRequest });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(authCalls.length, 2, "a console remains bound to the board it was opened for");
    await sync({ hardware: { ...client.state.hardware, serial_number: authRequest.uid.toUpperCase() } });
    await sync({ pending_request: { opcode: 1, transaction_id: 1 }, outcome_uncertain: true });
    assert.equal(el("#usb-open").disabled, true);
    await fire("#bluetooth-open", "click"); await null;
    assert.match(el("#bluetooth-connection-status").textContent, /Connected to Nearby PicoForge/);
    assert.equal(el("#direct-ble-control-key").disabled, true);
    assert.equal(el("#bluetooth-scan").textContent, "Disconnect", "one button toggles while connected");
    assert.equal(el("#bluetooth-scan").disabled, false);
    await fire("#bluetooth-scan", "click");
    assert.equal(client.transport.bleKey, null);
    assert.equal(el("#usb-open").disabled, true, "disconnect must not discard a pending command to switch transport");
    await sync({ outcome_uncertain: false, pending_request: null });
    assert.equal(el("#usb-open").disabled, false);
    el("#direct-ble-control-key").value = "ab".repeat(32);
    await fire("#bluetooth-connection-close", "click");
    assert.equal(el("#direct-ble-control-key").value, "", "closing the dialog discards an unsubmitted key");
    Object.defineProperty(globalThis, "navigator", { configurable: true, value: {} });
    await fire("#usb-open", "click"); await null;
    await fire("#bluetooth-open", "click"); await null;
    await sync({});
    assert.equal(el("#bluetooth-scan").disabled, true);
    assert.match(el("#bluetooth-connection-status").textContent, /supported browser/);
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: false });
    await sync({});
    assert.equal(el("#usb-connect").disabled, true);
    windowHandlers.get("pagehide")({ persisted: true });
    assert.equal(client.disposed, true, "cached pages still release board resources");
    windowHandlers.get("pageshow")({ persisted: true });
    assert.equal(reloads, 1, "history-cache restoration loads a fresh disconnected dashboard");
    assert.equal(scans, 2, "restoration must not reconnect or replay operations");
  } finally {
    for (const [name, method] of prototypes) BoardSession.prototype[name] = method;
    BleTransport.prototype.createConsoleProof = createConsoleProof;
    globalThis.document = saved.document;
    globalThis.window = saved.window;
    if (saved.location === undefined) delete globalThis.location;
    else globalThis.location = saved.location;
    for (const [name, descriptor] of [["navigator", saved.navigator], ["isSecureContext", saved.secure],
      ["sessionStorage", saved.sessionStorage], ["crypto", saved.crypto]]) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    }
  }
});
