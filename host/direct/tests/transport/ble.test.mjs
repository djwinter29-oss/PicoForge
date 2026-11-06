import assert from "node:assert/strict";
import test from "node:test";
import { createHmac, webcrypto } from "node:crypto";
import { describe } from "../../web/js/ui/messages.mjs";
import { BleTransport, BLE_SERVICE } from "../../web/js/transport/ble.mjs";
import { BoardSession } from "../../web/js/board/session.mjs";
import {
  fakeConnection, storage, selfTest, identity, config, telemetry, result, patchState, says,
} from "../support/fixtures.mjs";
import { status } from "../support/wireless.mjs";

test("BLE maps normalized68byte control to restricted9byte wire form and HMACs those exact bytes", async () => {
  const previous = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  const previousWarn = console.warn;
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: webcrypto });
  const setup = fakeConnection();
  const writes = [], requested = [], requestedServices = [], discoveryOrder = [];
  let notificationStarts = 0;
  let serviceChangedAvailable = true;
  const warnings = [];
  console.warn = (...args) => warnings.push(args);
  const nonce = Uint8Array.from({ length: 16 }, (_, index) => index);
  const ble = new EventTarget();
  const values = { 1: identity(128 | 55), 2: config(), 3: telemetry(), 10: storage(), 11: selfTest(),
    12: status(), 256: nonce };
  ble.gatt = { connected: false,
    async connect() {
      this.connected = true;
      return { async getPrimaryService(uuid) {
        if (uuid === 0x1801) {
          if (!serviceChangedAvailable) throw Object.assign(new Error("Service is hidden"), { name: "NotFoundError" });
          return { async getCharacteristic(uuid) {
            assert.equal(uuid, 0x2a05);
            return { async startNotifications() {
              notificationStarts++;
              discoveryOrder.push("service_changed_subscription");
            } };
          } };
        }
        assert.equal(uuid, BLE_SERVICE);
        discoveryOrder.push("pico_service_discovery");
        return { async getCharacteristic(uuid) {
          const id = parseInt(uuid.slice(4, 8), 16); requested.push(id);
          return {
            async readValue() {
              const request = writes.findLast(([id]) => id === 20)?.[1];
              const bytes = id === 20 ? result(request[1], request[2]).slice(0, 9) : values[id];
              if (id === 20 && request[2] === 5) bytes.set(request.slice(4, 8), 5);
              return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
            },
            async writeValueWithResponse(bytes) { writes.push([id, [...bytes]]); },
          };
        } };
      } };
    },
    disconnect() { this.connected = false; ble.dispatchEvent(new Event("gattserverdisconnected")); },
  };
  const client = new BoardSession(new BleTransport({ requestDevice: async options => {
    requestedServices.push(...options.optionalServices);
    return ble;
  } }),
    { pendingStore: setup.pendingStore, wirelessPolling: false });
  try {
    await client.connect(new Uint8Array(32).fill(0xab));
    assert.equal(client.state.connected, true);
    assert.ok(requestedServices.includes(0x1801));
    assert.equal(notificationStarts, 1);
    assert.deepEqual(discoveryOrder.slice(0, 2), ["service_changed_subscription", "pico_service_discovery"]);
    assert.ok(requested.includes(12)); assert.ok(requested.includes(256)); assert.ok(requested.includes(257));
    const sign = request => [...createHmac("sha256", Buffer.from("ab".repeat(32), "hex"))
      .update(Buffer.concat([nonce, Buffer.from("0102030405060708", "hex"), Buffer.from("PFBLECTL1"),
        Buffer.from(request)])).digest().subarray(0, 16)];
    assert.deepEqual(writes[0], [257, sign(Buffer.from("SESSION01"))]);
    const uid = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8]);
    assert.deepEqual(Buffer.from(await client.authorizeConsole(nonce, uid)),
      createHmac("sha256", Buffer.from("ab".repeat(32), "hex"))
        .update(Buffer.concat([nonce, uid, Buffer.from("PFTCPCLI1TCPCLI001")])).digest().subarray(0, 16));
    assert.equal(writes.length, 1, "BLE console authorization signs locally without a second board command");
    writes.length = 0;
    await client.command(1, { args: [1] });
    assert.deepEqual(writes, [[20, [1, 1, 1, 0, 1, 0, 0, 0, 0]]]);
    writes.length = 0;
    await client.command(2, { flags: 1 });
    assert.deepEqual(writes[1], [20, [1, 2, 2, 1, 0, 0, 0, 0, 0]]);
    assert.deepEqual(writes[0], [257, sign(writes[1][1])]);
    patchState(client, { reboot_pending: false });
    writes.length = 0;
    await client.command(5, { args: [200, 0, 1, 4] });
    assert.deepEqual(writes[1], [20, [1, 3, 5, 0, 200, 0, 1, 4, 0]]);
    assert.deepEqual(writes[0], [257, sign(writes[1][1])]);
    patchState(client, { reboot_pending: false });
    for (const opcode of [3, 4, 6, 7, 8, 9, 10, 11, 12, 13]) {
      await assert.rejects(client.command(opcode), says(/requires USB/));
    }
    await client.readWireless();
    assert.equal(client.state.wireless.ssid_hex, "");
    await client.disconnect();
    serviceChangedAvailable = false;
    await client.connect(new Uint8Array(32).fill(0xab));
    assert.equal(client.state.connected, true);
    assert.equal(notificationStarts, 1);
    assert.ok(warnings.some(([message]) => message.includes("Service Changed is unavailable")));
  } finally {
    await client.disconnect();
    console.warn = previousWarn;
    if (previous) Object.defineProperty(globalThis, "crypto", previous);
    else delete globalThis.crypto;
  }
});

test("BLE discovery retries only read-only setup on the chosen board and retains the final failure", async () => {
  const ble = new EventTarget();
  const values = { 1: identity(128 | 55), 2: config(), 3: telemetry(), 10: storage(), 11: selfTest(),
    12: status(), 256: new Uint8Array(16) };
  let failures = 1, connects = 0, prompts = 0;
  ble.gatt = { connected: false,
    async connect() {
      connects++; this.connected = true;
      return { async getPrimaryService(uuid) {
        if (uuid === 0x1801) {
          return { async getCharacteristic() {
            return { async startNotifications() {} };
          } };
        }
        if (failures-- > 0) throw new Error("No matching service");
        return { async getCharacteristic(uuid) {
          const bytes = values[parseInt(uuid.slice(4, 8), 16)] ?? new Uint8Array(9);
          return { readValue: async () => new DataView(bytes.buffer), writeValueWithResponse: async () => {} };
        } };
      } };
    },
    disconnect() { this.connected = false; ble.dispatchEvent(new Event("gattserverdisconnected")); },
  };
  const client = new BoardSession(new BleTransport({ requestDevice: async () => { prompts++; return ble; } },
    { sleep: async () => {} }),
    { pendingStore: fakeConnection().pendingStore, sleep: async () => {}, wirelessPolling: false });
  await client.connect(new Uint8Array(32).fill(0xab));
  assert.equal(client.state.connected, true);
  assert.deepEqual([prompts, connects], [1, 2]);
  await client.disconnect();
  failures = 3; connects = 0;
  await client.connect(new Uint8Array(32).fill(0xab));
  assert.equal(client.state.connected, false);
  assert.equal(connects, 3);
  assert.match(describe(client.state.error), /No matching service/);
  assert.equal(client.transport.bleKey, null);
});
