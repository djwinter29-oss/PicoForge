import assert from "node:assert/strict";
import { createHmac, webcrypto } from "node:crypto";
import test from "node:test";
import { PicoForgeBluetooth, BLE_SERVICE } from "../js/bluetooth.mjs";
import { buildWireless, buildWirelessCommand, decodeWireless, decodeWirelessResult, saveWireless } from "../js/wireless.mjs";
import { bindWirelessForm } from "../js/wireless-form.mjs";
import { fakeConnection, emit, storage, selfTest, identity, config, telemetry, result } from "./support/fixtures.mjs";

/** Board settings matching the config() fixture readback. */
const BOARD = { system_clock_mhz: 200, sdcard_profile: 1, radios: 0 };

test("v2 TCP switch, security masks and test lifecycle details retain legacy compatibility", () => {
  const settings = { wifi_enabled: false, ble_enabled: false, ssid: "", board: BOARD };
  assert.equal(buildWireless(1, settings)[3], 83);
  assert.equal(buildWireless(1, settings, false, false)[3], 3);
  assert.equal(buildWireless(1, { ...settings, tcp_cli_enabled: false })[3], 75);
  assert.equal(buildWireless(1, { ...settings, tcp_cli_enabled: true })[3], 67);
  assert.equal(buildWireless(1, { ...settings, tcp_cli_auth_required: true })[3], 51);
  assert.throws(() => buildWireless(1, { ...settings, tcp_cli_enabled: false }, false, false));
  assert.throws(() => buildWireless(1, { ...settings, tcp_cli_enabled: 0 }));
  const status = new Uint8Array(52);
  status[0] = 3;
  status[47] = 1;
  assert.equal(decodeWireless(status).tcp_cli_enabled, false);
  assert.equal(decodeWireless(status).tcp_cli_configurable, true);
  const statusV4 = new Uint8Array(53);
  statusV4[0] = 4;
  statusV4[47] = 2;
  statusV4[49] = 7;
  statusV4[52] = 1;
  assert.equal(decodeWireless(statusV4).tcp_cli_auth_configurable, true);
  assert.equal(decodeWireless(statusV4).reconnect_count, 7);
  assert.equal(decodeWireless(statusV4).restart_required, true);
  status[47] = 4;
  assert.throws(() => decodeWireless(status));
  const report = new Uint8Array(222);
  report.set([2, 1, 2, 0, 1]);
  report.set([1, 120], 6);
  const labels = ["Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN"];
  labels.forEach((label, mask) => {
    report[40] = mask;
    const network = decodeWirelessResult(report).networks[0];
    assert.equal(network.security, label);
    assert.equal(network.secured, Boolean(mask));
  });
  report[40] = 8;
  assert.throws(() => decodeWirelessResult(report));
  report.fill(0);
  report.set([2, 1, 6, 9]);
  assert.equal(decodeWirelessResult(report).phase, "Test connection lost");
  report[3] = 10;
  assert.equal(decodeWirelessResult(report).phase, "Test credentials expired");
  report[3] = 11;
  assert.throws(() => decodeWirelessResult(report));
  for (const state of [4, 5, 6]) {
    report.set([2, 1, state, 0]);
    assert.equal(decodeWirelessResult(report).page, 0);
  }
});

test("disabling a saved radio erases its credentials; Wi-Fi Enabled needs a network", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  for (const id of ["wifi-dialog", "ble-dialog", "settings-wifi", "settings-ble", "wifi-scan", "wifi-other",
    "wifi-connect", "wifi-cancel", "wifi-ssid", "wifi-password", "wifi-password-label", "wifi-manual-label",
    "wifi-networks", "wifi-status", "wireless-summary", "ble-generate", "ble-copy", "ble-cancel", "ble-secret",
    "ble-status"]) {
    elements.set(id, { value: "", textContent: "", hidden: false, disabled: false, children: [], handlers: {},
      options: ["1", "0", "configure"].map(value => ({ value, disabled: false })),
      addEventListener(name, callback) { this.handlers[name] = callback; }, showModal() {}, close() {},
      replaceChildren() {}, focus() {} });
  }
  globalThis.document = { querySelector: selector => elements.get(selector.slice(1)) };
  const el = id => elements.get(id);
  const pick = (id, value) => { el(id).handlers.focus(); el(id).value = value; el(id).handlers.input(); };
  try {
    el("settings-wifi").value = "1";
    el("settings-ble").value = "1";
    let saved = { ssid: "home", password_configured: true, wifi_enabled: true, ble_enabled: true,
      ble_key_configured: true };
    const form = bindWirelessForm({ load: async () => saved, supported: () => true, ready: () => true,
      scan: async () => [], test: async () => ({}), cancelTest: async () => {} });
    await form.load();
    assert.equal(form.dirty(), false);
    pick("settings-wifi", "0");
    pick("settings-ble", "0");
    assert.equal(form.dirty(), true, "turning a saved radio off is a pending erase");
    assert.deepEqual(form.request(), { wifi_enabled: false, ble_enabled: false, ssid: "", confirm_reboot: true,
      password: "", ble_key: "" });
    assert.equal(buildWireless(1, { ...form.request(), board: { system_clock_mhz: 0, sdcard_profile: 0,
      radios: 0 } })[3], 48, "erasing the BLE key also disables TCP authentication");
    pick("settings-ble", "1");
    assert.equal(Object.hasOwn(form.request(), "ble_key"), false, "Enabled keeps the saved key");
    saved = { ssid: "", password_configured: false, wifi_enabled: false, ble_enabled: false,
      ble_key_configured: false };
    form.reset();
    el("settings-wifi").value = "0";
    el("settings-ble").value = "0";
    await form.load();
    assert.equal(form.dirty(), false, "a radio that was already off has nothing to erase");
    assert.equal(el("settings-wifi").options[0].disabled, true, "Wi-Fi cannot be enabled without a network");
    assert.equal(el("settings-ble").options[0].disabled, false, "keyless Bluetooth stays view-only");
  } finally {
    globalThis.document = previous;
  }
});

test("wireless dialogs scan, stage tested credentials, and keep them until the combined save", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const ids = ["wifi-dialog", "ble-dialog", "settings-wifi", "settings-ble", "wifi-scan", "wifi-other",
    "wifi-connect", "wifi-cancel", "wifi-ssid", "wifi-password", "wifi-password-label",
    "wifi-manual-label", "wifi-networks", "wifi-status", "wireless-summary", "ble-generate", "ble-copy",
    "ble-cancel", "ble-secret", "ble-status"];
  for (const id of ids) {
    elements.set(id, { value: "", textContent: "", hidden: false, disabled: false, type: "password", open: false,
      handlers: {}, addEventListener(name, callback) { this.handlers[name] = callback; },
      showModal() { this.open = true; }, close() { this.open = false; }, replaceChildren() {},
      setAttribute() {}, append() {}, focus() {} });
  }
  {
    elements.get("settings-wifi").value = "1";
    elements.get("settings-ble").value = "0";
    for (const id of ["settings-wifi", "settings-ble"]) {
      elements.get(id).options = ["1", "0", "configure"].map(value => ({ value, disabled: false }));
    }
  }
  elements.get("wifi-networks").children = [];
  elements.get("wifi-networks").replaceChildren = function (...children) { this.children = children; };
  elements.get("wifi-networks").append = function (child) { this.children.push(child); };
  globalThis.document = { querySelector: selector => elements.get(selector.slice(1)), createElement: () => ({
    handlers: {}, addEventListener(name, callback) { this.handlers[name] = callback; },
    setAttribute() {}, append(...children) { this.children = children; },
  }) };
  const el = id => elements.get(id);
  const pick = (id, value) => { el(id).handlers.focus(); el(id).value = value; el(id).handlers.input(); };
  const fresh = { ssid: "saved", wifi_enabled: true, ble_enabled: false, ble_key_configured: false,
    wifi_state: "connected" };
  const scanned = [];
  const tested = [];
  let cancelled = 0;
  let loadCount = 0;
  let pendingTest = null;
  let reportPhase;
  let tick;
  let now = 0;
  const oldTimers = { setInterval: globalThis.setInterval, clearInterval: globalThis.clearInterval, now: Date.now };
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => { tick = null; };
  Date.now = () => now;
  try {
    const form = bindWirelessForm({ load: async () => { loadCount++; return fresh; },
      supported: () => true, ready: () => true,
      scan: async () => {
        scanned.push(true);
        return [{ ssid: "nearby", rssi: -40, secured: true, security: "WPA/RSN" }];
      },
      test: async (credentials, onProgress) => {
        tested.push(credentials);
        reportPhase = onProgress;
        return pendingTest ?? { ipv4: "192.168.1.9", remaining_seconds: 600 };
      },
      cancelTest: async () => { cancelled++; } });
    await form.load();
    assert.equal(el("wifi-ssid").value, "saved");
    assert.equal(form.dirty(), false);
    pick("settings-wifi", "configure");
    assert.equal(el("settings-wifi").value, "1", "Configure… opens the dialog and keeps the radio state");
    assert.equal(el("wifi-dialog").open, true);
    el("wifi-dialog").close();
    pick("settings-ble", "configure");
    assert.equal(el("settings-ble").value, "0");
    assert.equal(el("ble-dialog").open, true);
    el("ble-dialog").close();
    assert.equal(el("wireless-summary").textContent, "");
    assert.equal(el("wireless-summary").hidden, true, "live connection status belongs in diagnostics");
    await form.load();
    assert.equal(loadCount, 1, "wireless status should load once per board connection");
    pick("settings-wifi", "configure");
    await el("wifi-scan").handlers.click();
    assert.deepEqual(scanned, [true]);
    assert.match(el("wifi-networks").children[0].children[1].textContent, /WPA\/RSN/);
    el("wifi-networks").children[0].handlers.click();
    assert.equal(el("wifi-ssid").value, "nearby");
    await form.load();
    assert.equal(el("wifi-ssid").value, "nearby", "status refresh must not overwrite the user's selected network");
    el("wifi-password").value = "new-password";
    assert.equal(form.dirty(), false, "credentials are not staged until a test succeeds");
    await el("wifi-connect").handlers.click();
    assert.deepEqual(tested, [{ ssid: "nearby", password: "new-password" }]);
    assert.deepEqual(form.request(), { wifi_enabled: true, ble_enabled: false, ssid: "nearby",
      confirm_reboot: true, use_tested_wifi: true });
    assert.equal(form.dirty(), true);
    assert.equal(el("wireless-summary").textContent, "Wi-Fi “nearby” tested; expires in 600s.");
    assert.equal(el("wireless-summary").hidden, false);
    assert.equal(el("wifi-dialog").open, false);
    form.finish(null, "rejected");
    assert.equal(el("wifi-password").value, "");
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(el("wifi-status").textContent, "rejected");
    el("ble-generate").handlers.click();
    assert.match(el("ble-secret").value, /^[0-9a-f]{64}$/);
    assert.equal(el("settings-ble").value, "1", "a generated key enables Bluetooth");
    assert.equal(form.request().ble_enabled, true);
    form.finish({ ...fresh, ssid: "nearby", ble_enabled: true });
    assert.equal(el("ble-secret").value, "");
    assert.equal(el("ble-secret").type, "password");
    assert.equal(form.dirty(), false);
    pick("settings-wifi", "configure");
    assert.equal(el("wireless-summary").textContent, "");
    assert.equal(el("wireless-summary").hidden, true, "saving clears the staged-configuration notice");
    el("wifi-ssid").value = "second-network";
    el("wifi-password").value = "";
    await el("wifi-connect").handlers.click();
    assert.deepEqual(tested.at(-1), { ssid: "second-network", password: "" });
    assert.equal(form.request().use_tested_wifi, true);
    el("wifi-cancel").handlers.click();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(cancelled, 1);
    assert.equal(form.dirty(), false);
    pick("settings-wifi", "configure");
    el("wifi-ssid").value = "slow-network";
    let finishTest;
    pendingTest = new Promise(resolve => { finishTest = resolve; });
    const waiting = el("wifi-connect").handlers.click();
    assert.equal(form.operationBusy(), true);
    assert.equal(el("wifi-connect").textContent, "Testing…");
    assert.equal(el("wifi-cancel").disabled, true);
    assert.equal(el("wifi-password").disabled, true);
    now = 12000;
    reportPhase("Checking password");
    assert.match(el("wifi-status").textContent, /Checking password… 12s \(up to 30s\)/);
    now = 31000;
    tick();
    assert.match(el("wifi-status").textContent, /Checking password… 31s; waiting for the final result/);
    el("wifi-dialog").handlers.cancel({ preventDefault() {} });
    assert.equal(el("wifi-dialog").open, true, "Escape must not dismiss an in-flight test");
    finishTest({ ipv4: "192.168.1.10", remaining_seconds: 600 });
    await waiting;
    assert.equal(tick, null);
    assert.equal(form.operationBusy(), false);
    assert.equal(el("wifi-connect").textContent, "Test connection");
    assert.equal(el("wifi-cancel").disabled, false);
    assert.equal(form.request().ssid, "slow-network");

    pick("settings-wifi", "configure");
    pendingTest = new Promise(resolve => { finishTest = resolve; });
    const obsolete = el("wifi-connect").handlers.click();
    form.reset();
    finishTest({ ipv4: "192.168.1.10", remaining_seconds: 600 });
    await obsolete;
    assert.equal(form.dirty(), false, "a late result after disconnect must not stage credentials");
    assert.equal(tick, null);
  } finally {
    globalThis.document = previous;
    globalThis.setInterval = oldTimers.setInterval;
    globalThis.clearInterval = oldTimers.clearInterval;
    Date.now = oldTimers.now;
  }
});

test("wireless scan/test command and result reports have consistent bounded layouts", () => {
  const command = buildWirelessCommand(7, 3, { ssid: "network", password: "passphrase" });
  assert.equal(command.length, 101);
  assert.deepEqual([...command.slice(0, 6)], [1, 3, 7, 0, 7, 10]);
  assert.equal(new TextDecoder().decode(command.slice(6, 13)), "network");
  assert.equal(new TextDecoder().decode(command.slice(38, 48)), "passphrase");
  assert.deepEqual([...buildWirelessCommand(8, 2, { page: 2 }).slice(0, 4)], [1, 2, 8, 2]);
  assert.deepEqual([...buildWirelessCommand(9, 4).slice(0, 4)], [1, 4, 9, 0]);
  for (const settings of [{ ssid: "", password: "" }, { ssid: "x", password: "short" },
    { ssid: "x".repeat(33), password: "passphrase" }]) {
    assert.throws(() => buildWirelessCommand(1, 3, settings));
  }
  const report = new Uint8Array(222);
  report.set([1, 7, 2, 0, 1, 3]);
  report.set([4, ...new TextEncoder().encode("test")], 6);
  report[39] = 0xd8;
  report[40] = 1;
  const decoded = decodeWirelessResult(report);
  assert.equal(decoded.networks[0].ssid, "test");
  assert.equal(decoded.networks[0].rssi, -40);
  assert.equal(decoded.networks[0].secured, true);
  report[2] = 5;
  report.set([192, 168, 1, 9], 216);
  assert.equal(decodeWirelessResult(report).ipv4, "192.168.1.9");
  const failed = new Uint8Array(222);
  failed.set([1, 7, 6, 5]);
  assert.deepEqual([decodeWirelessResult(failed).detail, decodeWirelessResult(failed).phase], [5, "Password rejected"]);
  failed[3] = 8;
  assert.equal(decodeWirelessResult(failed).phase, "Could not start connection");
  failed[3] = 9;
  assert.throws(() => decodeWirelessResult(failed));
  report[4] = 19;
  assert.throws(() => decodeWirelessResult(report));
});

test("USB wireless payloads preserve omitted secrets and reject malformed credentials without disclosing them", () => {
  const board = { system_clock_mhz: 250, sdcard_profile: 1, radios: 3 };
  const settings = { wifi_enabled: true, ble_enabled: true, ssid: "test", password: "passphrase",
    ble_key: "ab".repeat(32), board };
  const request = buildWireless(7, settings);
  assert.equal(request.length, 136);
  assert.deepEqual([...request.slice(0, 4)], [2, 7, 3, 80]);
  assert.equal(request[101], 0xab);
  assert.deepEqual([...request.slice(133)], [250, 0, 1]);
  const keep = buildWireless(8, { wifi_enabled: false, ble_enabled: true, ssid: "", board: { ...board, radios: 2 } });
  assert.equal(keep[3], 83);
  for (const change of [{ ssid: "x".repeat(33) }, { ssid: "bad\0ssid" }, { password: "short" },
    { password: "é".repeat(8) }, { ble_key: "0".repeat(64) }, { wifi_enabled: 1 }, { board: undefined },
    { wifi_enabled: false },
    { board: { ...board, system_clock_mhz: 175 } }, { board: { ...board, radios: 4 } }]) {
    assert.throws(() => buildWireless(1, { ...settings, ...change }));
  }
  const bytes = new Uint8Array(52);
  bytes.set([3, 3, 2, 7, 2, 192, 168, 1, 9]);
  bytes.set(new TextEncoder().encode("test"), 9);
  bytes[42] = bytes[43] = 1;
  const decoded = decodeWireless(bytes);
  assert.equal(decoded.ssid, "test");
  assert.equal(decoded.ipv4, "192.168.1.9");
  assert.equal(decoded.password, undefined);
  assert.equal(decoded.ble_key, undefined);
  assert.equal(decoded.ble_state, "off");
  assert.equal(decoded.tcp_cli_started, false);
  bytes[45] = 2;
  assert.equal(decodeWireless(bytes).ble_state, "connected");
  bytes[45] = 0;
  bytes[46] = 1;
  assert.equal(decodeWireless(bytes).tcp_cli_started, true);
  for (const [offset, value] of [[0, 4], [1, 4], [2, 7], [4, 4], [42, 2], [44, 4], [45, 3], [46, 2], [47, 4]]) {
    const bad = bytes.slice(); bad[offset] = value;
    assert.throws(() => decodeWireless(bad));
  }
});

test("USB wireless save sends once, verifies sanitized readback, and locks writes on uncertainty", async () => {
  for (const failure of [false, true]) {
    const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
    await client.connect();
    emit(device, 10, storage()); emit(device, 11, selfTest());
    const receive = device.receiveFeatureReport;
    device.receiveFeatureReport = async id => {
      if (id !== 30) return receive(id);
      if (failure && sent.length) throw new Error("USB disconnected");
      const report = new Uint8Array(52); report.set([3, 0, 2, 1]);
      return report;
    };
    const operation = saveWireless(client, { wifi_enabled: false, ble_enabled: false, ssid: "", board: BOARD });
    if (failure) await assert.rejects(operation, /disconnected/);
    else await operation;
    assert.equal(sent.length, 1);
    assert.equal(sent[0][0], 31);
    assert.equal(client.state.outcome_uncertain, failure);
    assert.equal(Boolean(client.state.reboot_pending), !failure);
    if (!failure) assert.equal(client.state.settings.system_clock_mhz, 200);
    await client.disconnect();
  }
});

test("USB wireless save treats unconfirmed board settings as uncertain", async () => {
  const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id !== 30) return receive(id);
    const report = new Uint8Array(52); report.set([3, 1, 2, 1]);
    report.set(new TextEncoder().encode("test"), 9);
    return report;
  };
  await assert.rejects(saveWireless(client, { wifi_enabled: true, ble_enabled: false, ssid: "test",
    board: { ...BOARD, radios: 1 } }), /not confirmed/);
  assert.equal(sent.length, 1);
  assert.equal(client.state.outcome_uncertain, true);
  assert.equal(Boolean(client.state.reboot_pending), false);
  await client.disconnect();
});

test("USB wireless TCP switch save verifies readback and does not change board radio bits", async () => {
  for (const mismatch of [false, true]) {
    const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
    await client.connect();
    emit(device, 10, storage()); emit(device, 11, selfTest());
    const receive = device.receiveFeatureReport;
    device.receiveFeatureReport = async id => {
      if (id !== 30) return receive(id);
      const report = new Uint8Array(52);
      report.set([2, 0, 2, 1]);
      report[47] = sent.length && !mismatch ? 1 : 0;
      return report;
    };
    const save = saveWireless(client, { wifi_enabled: false, ble_enabled: false, ssid: "",
      tcp_cli_enabled: false, board: BOARD });
    if (mismatch) await assert.rejects(save, /did not confirm/);
    else assert.equal((await save).tcp_cli_enabled, false);
    assert.equal(sent.length, 1);
    assert.deepEqual(sent[0].slice(1, 5), [2, 1, 0, 11]);
    assert.equal(client.state.outcome_uncertain, mismatch);
    await client.disconnect();
  }
});

test("USB wireless save treats loss of a preserved secret as uncertain, never success", async () => {
  const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id !== 30) return receive(id);
    const report = new Uint8Array(52);
    report.set([1, 0, 2, 1]);
    report[42] = sent.length ? 0 : 1;
    return report;
  };
  await assert.rejects(saveWireless(client, { wifi_enabled: false, ble_enabled: false, ssid: "", board: BOARD }),
    /did not confirm/);
  assert.equal(sent.length, 1);
  assert.equal(client.state.outcome_uncertain, true);
  assert.equal(Boolean(client.state.reboot_pending), false);
  await client.disconnect();
});

test("BLE reads diagnostics without a key but authenticates each restricted write with a command-bound proof", async () => {
  const oldCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: webcrypto });
  let client;
  try {
    const ble = new EventTarget();
    const writes = [];
    const nonce = Uint8Array.from({ length: 16 }, (_, i) => i);
    const characteristics = new Map();
    for (const id of [1, 2, 3, 10, 11, 20, 30, 32, 33]) {
      characteristics.set(id, {
        async readValue() {
          const bytes = id === 1 ? identity(128 | 55) : id === 2 ? config() : id === 3 ? telemetry()
            : id === 10 ? storage() : id === 11 ? selfTest() : id === 32 ? nonce
              : result(writes.at(-1)?.[1]?.[1] ?? 1, 1);
          return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        },
        async writeValueWithResponse(bytes) { writes.push([id, [...bytes]]); },
      });
    }
    ble.gatt = { connected: false,
      async connect() {
        this.connected = true;
        return { async getPrimaryService(uuid) {
          assert.equal(uuid, BLE_SERVICE);
          return { async getCharacteristic(uuid) { return characteristics.get(parseInt(uuid.slice(4, 8), 16)); } };
        } };
      },
      disconnect() { this.connected = false; ble.dispatchEvent(new Event("gattserverdisconnected")); },
    };
    const bluetooth = { async requestDevice(options) {
      assert.deepEqual(options, { filters: [{ services: [BLE_SERVICE] }, { namePrefix: "PicoForge-" }], optionalServices: [BLE_SERVICE] });
      return ble;
    } };
    client = new PicoForgeBluetooth(bluetooth, () => {}, { sleep: async () => {} });
    await client.connect();
    assert.equal(client.state.connected, true);
    assert.equal(client.state.transport, "ble");
    assert.equal(client.accessMode, null, "access requires an explicit choice after connecting");
    await assert.rejects(client.command(1, { args: [1] }), /key/);
    assert.equal(writes.length, 0);
    client.setViewOnly();
    assert.equal(client.accessMode, "view-only");
    await assert.rejects(client.command(1, { args: [1] }), /Control/);
    assert.equal(writes.length, 0);
    await client.setControlKey("ab".repeat(32));
    assert.equal(client.accessMode, "control");
    await client.command(1, { args: [1] });
    assert.deepEqual(writes.map(item => item[0]), [33, 20]);
    const uid = Buffer.from(client.state.hardware.serial_number, "hex");
    const expected = createHmac("sha256", Buffer.from("ab".repeat(32), "hex"))
      .update(Buffer.concat([nonce, uid, Buffer.from(writes[1][1])])).digest().subarray(0, 16);
    assert.deepEqual(writes[0][1], [...expected]);
    for (const opcode of [2, 3, 6]) await assert.rejects(client.command(opcode), /requires USB/);
    assert.equal(writes.length, 2);
    characteristics.get(20).writeValueWithResponse = async () => { throw new Error("GATT Error Unknown."); };
    await assert.rejects(client.command(1, { args: [0] }), /rejected the BLE control key/);
    assert.equal(client.state.connected, true, "a rejected key must not drop the link");
    assert.equal(Boolean(client.state.outcome_uncertain), false);
    await client.disconnect();
    assert.equal(client.bleKey, null);
    assert.equal(client.accessMode, null);
    assert.equal(client.state.connected, false);
  } finally {
    if (client?.state.connected) await client.disconnect();
    if (oldCrypto) Object.defineProperty(globalThis, "crypto", oldCrypto);
    else delete globalThis.crypto;
  }
});
