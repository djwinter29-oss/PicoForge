import assert from "node:assert/strict";
import { createHmac, webcrypto } from "node:crypto";
import test from "node:test";
import { PicoForgeBluetooth, BLE_SERVICE } from "../js/bluetooth.mjs";
import { buildWireless, buildWirelessCommand, decodeWireless, decodeWirelessResult, saveWireless } from "../js/wireless.mjs";
import { bindWirelessForm } from "../js/wireless-form.mjs";
import { fakeConnection, emit, storage, selfTest, identity, config, telemetry, result } from "./support/fixtures.mjs";

/** Board settings matching the config() fixture readback. */
const BOARD = { system_clock_mhz: 200, sdcard_profile: 1, radios: 0 };

test("wireless dialogs scan, stage tested credentials, and keep them until the combined save", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const ids = ["wifi-dialog", "ble-dialog", "wireless-actions", "wifi-open", "ble-open", "wifi-scan", "wifi-other",
    "wifi-connect", "wifi-cancel", "wifi-ssid", "wifi-password", "wifi-password-label",
    "wifi-manual-label", "wifi-networks", "wifi-status", "wireless-summary", "ble-generate", "ble-copy",
    "ble-cancel", "ble-secret", "ble-status"];
  for (const id of ids) {
    elements.set(id, { value: "", textContent: "", hidden: false, disabled: false, type: "password", open: false,
      handlers: {}, addEventListener(name, callback) { this.handlers[name] = callback; },
      showModal() { this.open = true; }, close() { this.open = false; }, replaceChildren() {},
      setAttribute() {}, append() {}, focus() {} });
  }
  elements.get("wifi-networks").children = [];
  elements.get("wifi-networks").replaceChildren = function (...children) { this.children = children; };
  elements.get("wifi-networks").append = function (child) { this.children.push(child); };
  globalThis.document = { querySelector: selector => elements.get(selector.slice(1)), createElement: () => ({
    handlers: {}, addEventListener(name, callback) { this.handlers[name] = callback; },
    setAttribute() {}, append(...children) { this.children = children; },
  }) };
  const el = id => elements.get(id);
  const fresh = { ssid: "saved", wifi_enabled: true, ble_enabled: false, ble_key_configured: false,
    wifi_state: "connected" };
  const scanned = [];
  const tested = [];
  let cancelled = 0;
  let loadCount = 0;
  try {
    const form = bindWirelessForm({ load: async () => { loadCount++; return fresh; },
      supported: () => true, ready: () => true,
      scan: async () => { scanned.push(true); return [{ ssid: "nearby", rssi: -40, secured: true }]; },
      test: async credentials => { tested.push(credentials); return { ipv4: "192.168.1.9" }; },
      cancelTest: async () => { cancelled++; } });
    await form.load();
    assert.equal(el("wifi-ssid").value, "saved");
    assert.equal(form.dirty(), false);
    await form.load();
    assert.equal(loadCount, 1, "wireless status should load once per board connection");
    el("wifi-open").handlers.click();
    await el("wifi-scan").handlers.click();
    assert.deepEqual(scanned, [true]);
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
    assert.equal(el("wifi-dialog").open, false);
    form.finish(null, "rejected");
    assert.equal(el("wifi-password").value, "");
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(el("wifi-status").textContent, "rejected");
    el("ble-generate").handlers.click();
    assert.match(el("ble-secret").value, /^[0-9a-f]{64}$/);
    assert.equal(form.request().ble_enabled, true);
    form.finish({ ...fresh, ssid: "nearby", ble_enabled: true });
    assert.equal(el("ble-secret").value, "");
    assert.equal(el("ble-secret").type, "password");
    assert.equal(form.dirty(), false);
    el("wifi-open").handlers.click();
    el("wifi-ssid").value = "second-network";
    el("wifi-password").value = "";
    await el("wifi-connect").handlers.click();
    assert.deepEqual(tested.at(-1), { ssid: "second-network", password: "" });
    assert.equal(form.request().use_tested_wifi, true);
    el("wifi-cancel").handlers.click();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(cancelled, 1);
    assert.equal(form.dirty(), false);
  } finally { globalThis.document = previous; }
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
  const report = new Uint8Array(220);
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
  report[4] = 19;
  assert.throws(() => decodeWirelessResult(report));
});

test("USB wireless payloads preserve omitted secrets and reject malformed credentials without disclosing them", () => {
  const board = { system_clock_mhz: 250, sdcard_profile: 1, radios: 3 };
  const settings = { wifi_enabled: true, ble_enabled: true, ssid: "test", password: "passphrase",
    ble_key: "ab".repeat(32), board };
  const request = buildWireless(7, settings);
  assert.equal(request.length, 136);
  assert.deepEqual([...request.slice(0, 4)], [2, 7, 3, 0]);
  assert.equal(request[101], 0xab);
  assert.deepEqual([...request.slice(133)], [250, 0, 1]);
  const keep = buildWireless(8, { wifi_enabled: false, ble_enabled: true, ssid: "", board: { ...board, radios: 2 } });
  assert.equal(keep[3], 3);
  for (const change of [{ ssid: "x".repeat(33) }, { ssid: "bad\0ssid" }, { password: "short" },
    { password: "é".repeat(8) }, { ble_key: "0".repeat(64) }, { wifi_enabled: 1 }, { board: undefined },
    { wifi_enabled: false },
    { board: { ...board, system_clock_mhz: 175 } }, { board: { ...board, radios: 4 } }]) {
    assert.throws(() => buildWireless(1, { ...settings, ...change }));
  }
  const bytes = new Uint8Array(48);
  bytes.set([1, 3, 2, 7, 2, 192, 168, 1, 9]);
  bytes.set(new TextEncoder().encode("test"), 9);
  bytes[42] = bytes[43] = 1;
  const decoded = decodeWireless(bytes);
  assert.equal(decoded.ssid, "test");
  assert.equal(decoded.ipv4, "192.168.1.9");
  assert.equal(decoded.password, undefined);
  assert.equal(decoded.ble_key, undefined);
  for (const [offset, value] of [[0, 2], [1, 4], [2, 7], [4, 4], [42, 2], [44, 4], [47, 1]]) {
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
      const report = new Uint8Array(48); report.set([1, 0, 2, 1]);
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
    const report = new Uint8Array(48); report.set([1, 1, 2, 1]);
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

test("USB wireless save treats loss of a preserved secret as uncertain, never success", async () => {
  const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id !== 30) return receive(id);
    const report = new Uint8Array(48);
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
      assert.deepEqual(options, { filters: [{ services: [BLE_SERVICE] }] });
      return ble;
    } };
    client = new PicoForgeBluetooth(bluetooth, () => {}, { sleep: async () => {} });
    await client.connect();
    assert.equal(client.state.connected, true);
    assert.equal(client.state.transport, "ble");
    await assert.rejects(client.command(1, { args: [1] }), /key/);
    assert.equal(writes.length, 0);
    await client.setControlKey("ab".repeat(32));
    await client.command(1, { args: [1] });
    assert.deepEqual(writes.map(item => item[0]), [33, 20]);
    const uid = Buffer.from(client.state.hardware.serial_number, "hex");
    const expected = createHmac("sha256", Buffer.from("ab".repeat(32), "hex"))
      .update(Buffer.concat([nonce, uid, Buffer.from(writes[1][1])])).digest().subarray(0, 16);
    assert.deepEqual(writes[0][1], [...expected]);
    for (const opcode of [2, 3, 6]) await assert.rejects(client.command(opcode), /requires USB/);
    assert.equal(writes.length, 2);
    await client.disconnect();
    assert.equal(client.bleKey, null);
    assert.equal(client.state.connected, false);
  } finally {
    if (client?.state.connected) await client.disconnect();
    if (oldCrypto) Object.defineProperty(globalThis, "crypto", oldCrypto);
    else delete globalThis.crypto;
  }
});
