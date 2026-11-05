import assert from "node:assert/strict";
import { createHmac, webcrypto } from "node:crypto";
import { readFileSync } from "node:fs";
import test from "node:test";
import { PicoForgeBluetooth, BLE_SERVICE } from "../js/bluetooth.mjs";
import { PicoForgeWebHid, buildControlRequest, decodeControlResult } from "../js/device.mjs";
import {
  cancelWireless, decodeWireless, displaySsid, readWireless, registerWirelessRecovery,
  saveWireless, scanWireless, ssidBytes, testWireless,
} from "../js/wireless.mjs";
import { fakeConnection, emit, storage, selfTest, identity, config, telemetry, result } from "./support/fixtures.mjs";

const BOARD = { system_clock_mhz: 200, sdcard_profile: 1, radios: 0 };
const vectors = JSON.parse(readFileSync(new URL("../../../tests/fixtures/wireless-status.json", import.meta.url)));
function status({ state = 0, flags = 0, staged = 0, ssid = [], scanState = 0, scanGeneration = 0,
  count = 0, testGeneration = 0, detail = 0 } = {}) {
  const bytes = new Uint8Array(48);
  bytes.set([1, state, detail, 0, flags]);
  if ([2, 5].includes(state)) bytes.set([192, 168, 1, 9], 5);
  bytes[9] = ssid.length;
  bytes.set(ssid, 10);
  bytes.set([scanState, scanGeneration & 255, scanGeneration >> 8, count, testGeneration, staged], 42);
  return bytes;
}

async function connected() {
  let now = 0;
  const setup = fakeConnection({ capabilities: 128 | 55, now: () => now,
    sleep: async milliseconds => { now += milliseconds; } });
  await setup.client.connect();
  emit(setup.device, 10, storage()); emit(setup.device, 11, selfTest());
  registerWirelessRecovery(setup.client);
  return setup;
}

/** Stateful report double: opcodes mutate staging, accepted test/scan generations, and the last result slot. */
function radio(setup, { raw = new TextEncoder().encode("home"), scanEntries = [], testFailure = false } = {}) {
  let stagedMask = 0, stagedState = 0, tested = false, testGeneration = 0, scanGeneration = 0, scanning = false;
  let saved = raw.slice(), candidate = raw.slice();
  let last = result(0, 0, 0);
  const receive = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => {
    if (id === 12) return status({ ssid: candidate, state: testGeneration ? testFailure ? 6 : 5 : 0,
      flags: (saved.length ? 1 : 0) | stagedMask << 4 | (tested ? 128 : 0),
      staged: stagedState, testGeneration, detail: testFailure ? 3 : 0,
      scanGeneration, scanState: scanning ? 2 : 0, count: scanning ? scanEntries.length : 0 });
    if (id === 20) return last;
    return receive(id);
  };
  const send = setup.device.sendFeatureReport;
  setup.device.sendFeatureReport = async (id, bytes) => {
    assert.equal(id, 20); assert.equal(bytes.length, 68);
    await send(id, bytes);
    const op = bytes[2], args = bytes.subarray(4);
    last = result(bytes[1], op);
    if (op === 6) {
      candidate = args.slice(1, args[0] + 1);
      stagedMask |= 1; tested = false;
      if (!candidate.length) { stagedMask |= 2; stagedState |= 3; }
    } else if (op === 7) {
      stagedMask |= 2; tested = false;
      stagedState = args[0] ? stagedState & ~2 : stagedState | 2;
    } else if (op === 8) {
      stagedMask |= 4; stagedState = args[0] ? stagedState & ~4 : stagedState | 4;
    } else if (op === 9) { testGeneration = testGeneration % 255 + 1; tested = !testFailure; }
    else if (op === 10) { stagedMask = 0; stagedState = 0; tested = false; testGeneration = 0; candidate = saved; }
    else if (op === 11) { scanning = true; scanGeneration = scanGeneration % 65535 + 1; }
    else if (op === 12) {
      const entry = scanEntries[args[0]];
      last.set([scanGeneration & 255, scanGeneration >> 8, args[0], entry.length], 5);
      last.set(entry, 9); last[41] = 216; last[42] = 5;
    } else if (op === 5) {
      last.set(args.subarray(0, 4), 5); last[9] = stagedMask;
      saved = candidate; stagedMask = 0; stagedState = 0; tested = false;
    } else if (op === 13) last.fill(42, 5, 21);
  };
  return { replaceResult(value) { last = value; } };
}

test("Report12 uses exact48byte vectors, raw SSIDs and no fabricated removed diagnostics", () => {
  const wireless = decodeWireless(Buffer.from(vectors.tcp_cli_auth_required, "hex"),
    { radios: 3, tcp_cli_enabled: true });
  assert.equal(wireless.ssid, "PicoForge");
  assert.equal(wireless.ssid_hex, "5069636f466f726765");
  assert.equal(wireless.tcp_cli_started, true);
  assert.equal(wireless.tcp_cli_auth_required, true);
  assert.equal(wireless.wifi_enabled, true);
  for (const field of ["rssi", "reconnect_count", "wifi_auth_state", "restart_required", "password_configured",
    "transaction_id"]) assert.equal(Object.hasOwn(wireless, field), false, field);
  const candidate = decodeWireless(Buffer.from(vectors.tested_candidate, "hex"));
  assert.equal(candidate.pair_tested, true);
  assert.equal(candidate.test_generation, 1);
  const lost = decodeWireless(Buffer.from(vectors.test_failed_saved_link_serving, "hex"));
  assert.deepEqual([lost.wifi_state, lost.ipv4, lost.pair_tested, lost.tcp_cli_started],
    ["test_failed", "42.42.42.42", true, true]);
  const init = decodeWireless(Buffer.from(vectors.radio_init_failed_while_connected, "hex"));
  assert.deepEqual([init.wifi_state, init.detail], ["connected", 1]);
  assert.equal(candidate.wifi_state, "test_joined");
  for (const bad of [new Uint8Array(53), status().slice(1), new Uint8Array(49)]) {
    assert.throws(() => decodeWireless(bad));
  }
  for (const [offset, value] of [[0, 2], [1, 7], [3, 3], [9, 33], [10, 1], [42, 4], [45, 19], [47, 1]]) {
    const bad = status(); bad[offset] = value;
    assert.throws(() => decodeWireless(bad), `${offset}/${value}`);
  }
  const reserved = status(); reserved[2] = 255; reserved[4] = 8; reserved[47] = 128;
  assert.equal(decodeWireless(reserved).detail, 0, "reserved detail/flag bits are ignored");
});

test("opaque SSIDs escape invalid UTF8 and controls but round-trip raw bytes rather than the label", () => {
  const raw = Uint8Array.from([0x61, 0, 255, 0xc3, 0xa9, 0x3c, 10]);
  const wireless = decodeWireless(status({ ssid: raw }));
  assert.equal(wireless.ssid, "a\\x00\\xFFé<\\x0A");
  assert.equal(wireless.ssid_hex, "6100ffc3a93c0a");
  assert.deepEqual(ssidBytes({ ssid: wireless.ssid, ssid_hex: wireless.ssid_hex.toUpperCase() }), raw);
  assert.equal(displaySsid(Uint8Array.from([0xe2, 0x80, 0xae])), "\\xE2\\x80\\xAE");
  assert.throws(() => ssidBytes({ ssid_hex: "bad" }));
  assert.throws(() => ssidBytes({ ssid_hex: "aa".repeat(33) }));
  assert.throws(() => ssidBytes({ ssid: "é".repeat(17) }));
});

test("staging, scan index/generation and proof requests use64argument bytes with zero padding", () => {
  for (const [op, args] of [[6, [3, 0, 255, 1]], [7, [8, ...new TextEncoder().encode("password")]],
    [8, [32, ...Array(32).fill(0xab)]], [9, []], [10, []], [11, []], [12, [17, 0x34, 0x12]],
    [13, Array(16).fill(123)]]) {
    const request = buildControlRequest(255, op, { args });
    assert.equal(request.length, 68);
    assert.deepEqual([...request.slice(0, 4 + args.length)], [1, 255, op, 0, ...args]);
    assert.ok(request.subarray(4 + args.length).every(byte => !byte));
  }
  for (const [op, args] of [[6, [33]], [7, [7, ...Array(7).fill(65)]], [7, [8, ...Array(8).fill(0)]],
    [8, [31]], [9, [1]], [12, [18, 1]], [12, [0, 0, 0]], [13, [...Array(16).fill(1), 1]]]) {
    assert.throws(() => buildControlRequest(1, op, { args }));
  }
});

test("strict result validation enforces details, scan padding, commit masks and opcode status rules", () => {
  for (const [op, state, error, detail] of [[6, 3, 3, 16], [7, 3, 3, 17], [8, 3, 3, 18],
    [9, 3, 3, 19], [5, 3, 3, 20], [9, 3, 3, 21], [12, 3, 3, 22], [12, 3, 3, 23],
    [13, 3, 3, 24], [8, 5, 5, 25], [11, 5, 5, 26], [5, 3, 3, 27], [5, 5, 5, 28]]) {
    const bytes = result(1, op, state, error); bytes[5] = detail;
    assert.equal(decodeControlResult(bytes, { ble: detail === 28 }).detail, detail);
    if (detail === 28) assert.throws(() => decodeControlResult(bytes));
    bytes[6] = 1; assert.throws(() => decodeControlResult(bytes));
    bytes[6] = 0; bytes[2] = 1; assert.throws(() => decodeControlResult(bytes));
  }
  for (const op of [6, 7, 8, 10, 12, 13]) assert.throws(() => decodeControlResult(result(1, op, 1)));
  const scan = result(1, 12); scan.set([1, 0, 0, 1, 255], 5); scan[41] = 216; scan[42] = 7;
  assert.doesNotThrow(() => decodeControlResult(scan));
  for (const [offset, value] of [[5, 0], [7, 18], [8, 33], [10, 1], [42, 8], [43, 1]]) {
    const bad = scan.slice(); bad[offset] = value; assert.throws(() => decodeControlResult(bad));
  }
  const saved = result(1, 5); saved.set([200, 0, 1, 7, 7], 5);
  assert.equal(decodeControlResult(saved).commit_mask, 7);
  saved[9] = 8; assert.throws(() => decodeControlResult(saved));
});

test("scan lifecycle reads generation-indexed entries, keeps raw SSIDs and serializes the whole workflow", async () => {
  const setup = await connected();
  radio(setup, { scanEntries: [Uint8Array.from([0, 255, 65]), new TextEncoder().encode("normal")] });
  const pending = scanWireless(setup.client);
  await assert.rejects(setup.client.tcpCliAuthProof(new Uint8Array(16),
    Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8])), /unavailable/);
  const entries = await pending;
  assert.equal(entries[0].ssid_hex, "00ff41");
  assert.equal(entries[0].ssid, "\\x00\\xFFA");
  assert.equal(entries[0].rssi, -40);
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [11, 12, 12]);
  assert.deepEqual(setup.sent[1].slice(5, 8), [0, 1, 0]);
  assert.deepEqual(setup.sent[2].slice(5, 8), [1, 1, 0]);
  assert.equal(setup.client.pendingWirelessCommand, null);
  await setup.client.disconnect();
});

test("tested pair saves without restaging, key staging retains test, "
  + "save confirms exact switches and commit mask", async () => {
  const setup = await connected();
  const raw = Uint8Array.from([65, 0, 255]);
  radio(setup);
  const tested = await testWireless(setup.client, { ssid: "display only", ssid_hex: "4100FF", password: "password" });
  assert.ok(tested.remaining_seconds > 0 && tested.remaining_seconds <= 600);
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 7, 9]);
  assert.deepEqual(setup.sent[0].slice(5, 9), [3, ...raw]);
  const saved = await saveWireless(setup.client, { ssid: tested.ssid, ssid_hex: tested.ssid_hex,
    use_tested_wifi: true, wifi_enabled: true, ble_enabled: true, tcp_cli_enabled: true,
    ble_key: "ab".repeat(32), board: { ...BOARD, radios: 3 } });
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 7, 9, 8, 5]);
  assert.deepEqual(setup.sent.at(-1).slice(1, 9), [1, 5, 5, 2, 200, 0, 1, 7]);
  assert.equal(saved.ssid_hex, "4100ff");
  assert.equal(saved.ssid, "A\\x00\\xFF");
  assert.equal(setup.client.state.reboot_pending, true);
  assert.equal(setup.client.state.outcome_uncertain, false);
  assert.equal(setup.pendingStore.getItem("picoforge.direct.pending-operation"), null);
  await setup.client.disconnect();
});

test("password omission is safe only for the saved raw SSID; "
  + "explicit open/forget and cancel are supported", async () => {
  const setup = await connected();
  radio(setup);
  await assert.rejects(testWireless(setup.client, { ssid: "different" }), /explicit password/);
  assert.equal(setup.sent.length, 0);
  await testWireless(setup.client, { ssid: "home" });
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 9]);
  await cancelWireless(setup.client);
  assert.equal(setup.sent.at(-1)[3], 10);
  assert.equal(setup.client.state.wireless.staged_mask, 0);
  await testWireless(setup.client, { ssid: "open", password: "" });
  assert.deepEqual(setup.sent.at(-2).slice(3, 7), [7, 0, 0, 0]);
  await cancelWireless(setup.client);
  await saveWireless(setup.client, { ssid: "", password: "", wifi_enabled: false, ble_enabled: false, board: BOARD });
  assert.deepEqual(setup.sent.slice(-2).map(bytes => bytes[3]), [6, 5], "empty SSID implicitly clears password");
  await setup.client.disconnect();
});

test("failed tests leave discardable staging but do not claim a tested pair", async () => {
  const setup = await connected(); radio(setup, { testFailure: true });
  await assert.rejects(testWireless(setup.client, { ssid: "home", password: "password" }), /Password rejected/);
  assert.equal(setup.client.state.outcome_uncertain, false);
  assert.equal(setup.client.pendingWirelessCommand, null);
  await cancelWireless(setup.client);
  await setup.client.disconnect();
});

test("uncertain command recovery stores no secrets, reads same UID+tx+opcode and never replays SET", async () => {
  const setup = await connected(); radio(setup);
  const read = setup.device.receiveFeatureReport;
  let fail = true;
  setup.device.receiveFeatureReport = async id => {
    if (id === 20 && fail) throw new Error("USB disconnected");
    return read(id);
  };
  await assert.rejects(testWireless(setup.client, { ssid: "secret-network", password: "secret-password" }),
    /disconnected/);
  const raw = setup.pendingStore.getItem("picoforge.direct.pending-operation");
  assert.ok(raw);
  assert.equal(raw.includes("secret"), false);
  const record = JSON.parse(raw);
  assert.deepEqual(record.request, { transaction_id: 1, opcode: 6 });
  fail = false;
  const restored = new PicoForgeWebHid(setup.hid, () => {}, { pendingStore: setup.pendingStore });
  registerWirelessRecovery(restored);
  await restored.attachDevice(setup.device);
  assert.equal(restored.state.outcome_uncertain, false);
  assert.equal(setup.sent.length, 1);
  await restored.disconnect(); await setup.client.disconnect();
});

test("accepted test lifecycle survives reload, enforces generation and never stores pair bytes", async () => {
  for (const wrongGeneration of [false, true]) {
    const setup = await connected(); radio(setup);
    const read = setup.device.receiveFeatureReport;
    let interrupt = true;
    setup.device.receiveFeatureReport = async id => {
      if (id === 12 && setup.sent.some(bytes => bytes[3] === 9) && interrupt) throw new Error("Link lost");
      const bytes = await read(id);
      if (id === 12 && wrongGeneration) bytes[46] = 2;
      return bytes;
    };
    await assert.rejects(testWireless(setup.client, { ssid: "home", password: "password" }), /Link lost/);
    const raw = setup.pendingStore.getItem("picoforge.direct.pending-operation");
    assert.equal(raw.includes("password"), false);
    assert.equal(JSON.parse(raw).kind, "wireless-lifecycle");
    interrupt = false;
    const restored = new PicoForgeWebHid(setup.hid, () => {}, { pendingStore: setup.pendingStore });
    registerWirelessRecovery(restored);
    await restored.attachDevice(setup.device);
    assert.equal(restored.state.outcome_uncertain, wrongGeneration);
    assert.equal(setup.sent.length, 3);
    await restored.disconnect(); await setup.client.disconnect();
  }
});

test("save commit-mask mismatch is uncertain even after firmware clears staged state", async () => {
  const setup = await connected(); radio(setup);
  const read = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => {
    const bytes = await read(id);
    if (id === 20 && bytes[2] === 5) bytes[9] = 0;
    return bytes;
  };
  await assert.rejects(saveWireless(setup.client, { ssid: "", password: "", wifi_enabled: false,
    ble_enabled: false, board: BOARD }), /did not confirm/);
  assert.equal(setup.client.state.outcome_uncertain, true);
  await assert.rejects(setup.client.command(1), /uncertain/);
  assert.equal(JSON.parse(setup.pendingStore.getItem("picoforge.direct.pending-operation")).request.commit_mask, 3);
  await setup.client.disconnect();
});

test("scan result index/generation mismatch keeps the original query persisted and blocks further writes", async () => {
  const setup = await connected();
  radio(setup, { scanEntries: [Uint8Array.of(65)] });
  const read = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => {
    const bytes = await read(id);
    if (id === 20 && bytes[2] === 12) bytes[5] = 2;
    return bytes;
  };
  await assert.rejects(scanWireless(setup.client), /generation and index/);
  assert.equal(setup.client.state.outcome_uncertain, true);
  const request = JSON.parse(setup.pendingStore.getItem("picoforge.direct.pending-operation")).request;
  assert.equal(request.scan_generation, 1); assert.equal(request.scan_index, 0);
  await setup.client.disconnect();
});

test("Report12 freshness uses active500ms/idle2s and invalid reads never refresh the receipt", async () => {
  let now = 0;
  const setup = fakeConnection({ capabilities: 128 | 55, now: () => now });
  await setup.client.connect();
  let current = status();
  const read = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => id === 12 ? current : read(id);
  await readWireless(setup.client);
  now = 5999; setup.client.expireReports(); assert.ok(setup.client.state.wireless);
  now = 6000; setup.client.expireReports(); assert.equal(setup.client.state.wireless, null);
  current = status({ state: 4, flags: 48, testGeneration: 1 });
  await readWireless(setup.client);
  assert.equal(setup.client.wirelessInterval, 500);
  current = new Uint8Array(53);
  now = 7499; await assert.rejects(readWireless(setup.client));
  setup.client.expireReports(); assert.ok(setup.client.state.wireless);
  now = 7500; setup.client.expireReports(); assert.equal(setup.client.state.wireless, null);
  await setup.client.disconnect();
});

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
        if (uuid === "generic_attribute") {
          if (!serviceChangedAvailable) throw Object.assign(new Error("Service is hidden"), { name: "NotFoundError" });
          return { async getCharacteristic(uuid) {
            assert.equal(uuid, "service_changed");
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
  const client = new PicoForgeBluetooth({ requestDevice: async options => {
    requestedServices.push(...options.optionalServices);
    return ble;
  } }, () => {},
    { pendingStore: setup.pendingStore });
  try {
    await client.connect("ab".repeat(32));
    assert.equal(client.state.connected, true);
    assert.ok(requestedServices.includes("generic_attribute"));
    assert.equal(notificationStarts, 1);
    assert.deepEqual(discoveryOrder.slice(0, 2), ["service_changed_subscription", "pico_service_discovery"]);
    assert.ok(requested.includes(12)); assert.ok(requested.includes(256)); assert.ok(requested.includes(257));
    const sign = request => [...createHmac("sha256", Buffer.from("ab".repeat(32), "hex"))
      .update(Buffer.concat([nonce, Buffer.from("0102030405060708", "hex"), Buffer.from("PFBLECTL1"),
        Buffer.from(request)])).digest().subarray(0, 16)];
    assert.deepEqual(writes[0], [257, sign(Buffer.from("SESSION01"))]);
    writes.length = 0;
    await client.command(1, { args: [1] });
    assert.deepEqual(writes, [[20, [1, 1, 1, 0, 1, 0, 0, 0, 0]]]);
    writes.length = 0;
    await client.command(2, { flags: 1 });
    assert.deepEqual(writes[1], [20, [1, 2, 2, 1, 0, 0, 0, 0, 0]]);
    assert.deepEqual(writes[0], [257, sign(writes[1][1])]);
    client.update({ reboot_pending: false });
    writes.length = 0;
    await client.command(5, { args: [200, 0, 1, 4] });
    assert.deepEqual(writes[1], [20, [1, 3, 5, 0, 200, 0, 1, 4, 0]]);
    assert.deepEqual(writes[0], [257, sign(writes[1][1])]);
    client.update({ reboot_pending: false });
    for (const opcode of [3, 4, 6, 7, 8, 9, 10, 11, 12, 13]) {
      await assert.rejects(client.command(opcode), /requires USB/);
    }
    await readWireless(client);
    assert.equal(client.state.wireless.ssid_hex, "");
    await client.disconnect();
    serviceChangedAvailable = false;
    await client.connect("ab".repeat(32));
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
        if (uuid === "generic_attribute") {
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
  const client = new PicoForgeBluetooth({ requestDevice: async () => { prompts++; return ble; } }, () => {},
    { pendingStore: fakeConnection().pendingStore, sleep: async () => {} });
  await client.connect("ab".repeat(32));
  assert.equal(client.state.connected, true);
  assert.deepEqual([prompts, connects], [1, 2]);
  await client.disconnect();
  failures = 3; connects = 0;
  await client.connect("ab".repeat(32));
  assert.equal(client.state.connected, false);
  assert.equal(connects, 3);
  assert.match(client.state.error, /No matching service/);
  assert.equal(client.bleKey, null);
});

test("malformed save settings and credentials reject before the first SET", async () => {
  const setup = await connected(); radio(setup);
  const base = { ssid: "home", wifi_enabled: true, ble_enabled: false, board: { ...BOARD, radios: 1 } };
  for (const change of [{ board: { ...base.board, system_clock_mhz: undefined } },
    { board: { ...base.board, sdcard_profile: 2 } }, { use_tested_wifi: 1 }, { tcp_cli_enabled: 1 },
    { ble_key: "0".repeat(63) }, { password: "short" }, { password: "é".repeat(8) }, { ssid_hex: "not hex" }]) {
    await assert.rejects(saveWireless(setup.client, { ...base, ...change }));
    assert.equal(setup.sent.length, 0);
  }
  await setup.client.disconnect();
});

test("all-zero32byte keys remain configured; only empty keys clear", async () => {
  for (const key of ["0".repeat(64), ""]) {
    const setup = await connected(); radio(setup);
    try {
      const saved = await saveWireless(setup.client, { ssid: "home", wifi_enabled: true, ble_enabled: true,
        ble_key: key, board: { ...BOARD, radios: 3 } });
      assert.equal(saved.ble_key_configured, key !== "");
      assert.deepEqual(setup.sent.map(bytes => bytes[3]), [8, 5]);
      assert.equal(setup.sent[0][5], key === "" ? 0 : 32, "presence is determined by length, not byte values");
      assert.ok(setup.sent[0].slice(6).every(byte => byte === 0));
    } finally { await setup.client.disconnect(); }
  }
  const client = new PicoForgeBluetooth(undefined, () => {}, { pendingStore: fakeConnection().pendingStore });
  await client.setControlKey("0".repeat(64));
  assert.ok(client.bleKey, "an all-zero key can authorize a Bluetooth session");
  const message = Uint8Array.of(1, 2, 3);
  assert.deepEqual(Buffer.from(await crypto.subtle.sign("HMAC", client.bleKey, message)),
    createHmac("sha256", Buffer.alloc(32)).update(message).digest());
  await assert.rejects(client.setControlKey(""), /64 hex/);
});
