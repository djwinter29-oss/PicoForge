import assert from "node:assert/strict";
import { createHmac, webcrypto } from "node:crypto";
import test from "node:test";
import { PicoForgeBluetooth, BLE_SERVICE } from "../js/bluetooth.mjs";
import { buildWireless, decodeWireless, saveWireless } from "../js/wireless.mjs";
import { bindWirelessForm } from "../js/wireless-form.mjs";
import { fakeConnection, emit, storage, selfTest, identity, config, telemetry, result } from "./support/fixtures.mjs";

test("wireless form cancels without writing, preserves edits on refresh and clears secrets on reset/save", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  for (const id of ["wireless-panel", "wifi-enabled", "ble-enabled", "wifi-ssid", "wifi-password", "ble-secret",
    "wireless-save", "wireless-generate", "wireless-status", "wireless-dialog", "wireless-cancel", "wireless-confirm"]) {
    elements.set(id, { value: "", handlers: {}, open: false,
      addEventListener(name, callback) { this.handlers[name] = callback; },
      showModal() { this.open = true; }, close() { this.open = false; } });
  }
  globalThis.document = { querySelector: selector => elements.get(selector.slice(1)) };
  const el = id => elements.get(id);
  let writes = 0;
  let ready = true;
  let resolveLoad;
  const fresh = { ssid: "saved", wifi_enabled: true, ble_enabled: false, wifi_state: "connected" };
  let load = async () => fresh;
  try {
    const form = bindWirelessForm({ load: () => load(), supported: () => true, ready: () => ready,
      async save(config) { writes++; assert.equal(config.password, "new-password"); return fresh; } });
    await form.load();
    assert.equal(el("wifi-ssid").value, "saved");
    el("wifi-ssid").value = "edited";
    el("wifi-ssid").handlers.input();
    await form.load();
    assert.equal(el("wifi-ssid").value, "edited");
    el("wireless-save").handlers.click();
    el("wireless-cancel").handlers.click();
    await el("wireless-confirm").handlers.click();
    assert.equal(writes, 0);
    el("wireless-save").handlers.click();
    ready = false;
    await el("wireless-confirm").handlers.click();
    assert.equal(writes, 0);
    ready = true;
    el("wifi-password").value = "new-password";
    el("wireless-save").handlers.click();
    await el("wireless-confirm").handlers.click();
    assert.equal(writes, 1);
    assert.equal(el("wifi-password").value, "");
    load = () => new Promise(resolve => { resolveLoad = resolve; });
    const loading = form.load();
    el("ble-secret").value = "private";
    form.reset();
    resolveLoad(fresh);
    await loading;
    assert.equal(el("ble-secret").value, "");
    assert.equal(el("wifi-ssid").value, "");
  } finally { globalThis.document = previous; }
});

test("USB wireless payloads preserve omitted secrets and reject malformed credentials without disclosing them", () => {
  const settings = { wifi_enabled: true, ble_enabled: true, ssid: "test", password: "passphrase",
    ble_key: "ab".repeat(32) };
  const request = buildWireless(7, settings);
  assert.equal(request.length, 136);
  assert.deepEqual([...request.slice(0, 4)], [1, 7, 3, 0]);
  assert.equal(request[101], 0xab);
  const keep = buildWireless(8, { wifi_enabled: false, ble_enabled: false, ssid: "" });
  assert.equal(keep[3], 3);
  for (const change of [{ ssid: "x".repeat(33) }, { ssid: "bad\0ssid" }, { password: "short" },
    { password: "é".repeat(8) }, { ble_key: "0".repeat(64) }, { wifi_enabled: 1 }]) {
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
    const operation = saveWireless(client, { wifi_enabled: false, ble_enabled: false, ssid: "" });
    if (failure) await assert.rejects(operation, /disconnected/);
    else await operation;
    assert.equal(sent.length, 1);
    assert.equal(sent[0][0], 31);
    assert.equal(client.state.outcome_uncertain, failure);
    assert.equal(Boolean(client.state.reboot_pending), !failure);
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
    const report = new Uint8Array(48);
    report.set([1, 0, 2, 1]);
    report[42] = sent.length ? 0 : 1;
    return report;
  };
  await assert.rejects(saveWireless(client, { wifi_enabled: false, ble_enabled: false, ssid: "" }),
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
