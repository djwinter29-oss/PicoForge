import assert from "node:assert/strict";
import test from "node:test";
import { commandPending } from "../../web/js/state/selectors.mjs";
import { OP } from "../../web/js/protocol/constants.mjs";
import { describe } from "../../web/js/ui/messages.mjs";
import { BoardSession } from "../../web/js/board/session.mjs";
import { HidTransport } from "../../web/js/transport/hid.mjs";
import { config, telemetry, storage, selfTest, result, fakeConnection, emit, says } from "../support/fixtures.mjs";
import { BOARD, status, connected, radio } from "../support/wireless.mjs";

test("unavailable recovery storage fails closed and keeps its error visible across disconnect", () => {
  const unavailable = new BoardSession(new HidTransport(null), { pendingStore: null });
  assert.equal(unavailable.state.outcome_uncertain, true);
  assert.match(describe(unavailable.state.error), /session storage is unavailable/);
  unavailable.connection.clear();
  assert.equal(unavailable.state.outcome_uncertain, true);
  assert.match(describe(unavailable.state.error), /session storage is unavailable/);
});

test("malformed saved operation records fail closed and cannot authorize a write", async () => {
  const { device, hid, sent, pendingStore } = fakeConnection();
  pendingStore.setItem("picoforge.direct.pending-operation", JSON.stringify({
    version: 1, kind: "control", uid: "0102030405060708",
    request: { transaction_id: 1, opcode: 1 }, password: "unexpected",
  }));
  const client = new BoardSession(new HidTransport(hid), { sleep: async () => {}, pendingStore });
  assert.match(describe(client.state.error), /invalid/);
  await client.connect();
  emit(device, 3, telemetry());
  await assert.rejects(client.setLed(true), says(/unavailable/));
  assert.equal(sent.length, 0);
});

test("transactions serialize, match ID and opcode, poll Pending, wrap IDs, and do not mutate telemetry", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect(); emit(device, 3, telemetry());
  let calls = 0;
  const original = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id !== 20) return original(id);
    calls++;
    const [transaction, opcode] = sent.at(-1).slice(2, 4);
    if (calls === 1) return result(transaction, 3);
    if (calls === 2) return result(255, opcode);
    return result(transaction, opcode, calls === 3 ? 1 : 2);
  };
  client.commands.transaction = 255;
  const pending = client.setLed(true);
  await assert.rejects(client.setLed(true), says(/unavailable/));
  await pending;
  assert.deepEqual(sent[0], [20, 1, 1, 1, 0, 1, ...Array(63).fill(0)]);
  assert.equal(calls, 4);
  assert.equal(client.state.runtime.led_on, false, "acceptance is not an Input snapshot");
  await client.disconnect();
});

test("SET failure never polls; terminal rejection permits a new transaction; timeout blocks replay", async () => {
  const { client, device, reads } = fakeConnection();
  await client.connect(); emit(device, 3, telemetry());
  device.sendFeatureReport = async () => { throw new Error("SET rejected"); };
  await assert.rejects(client.setLed(true), says(/SET rejected/));
  assert.deepEqual(reads, [1, 2]);
  assert.equal(client.state.outcome_uncertain, false);
  let request;
  device.sendFeatureReport = async (id, payload) => { request = payload; };
  device.receiveFeatureReport = async () => result(request[1], request[2], 6, 6);
  await assert.rejects(client.setLed(true), says(/status 6, error 6/));
  assert.equal(client.state.outcome_uncertain, false);
  device.receiveFeatureReport = async () => result(request[1], request[2]);
  await assert.doesNotReject(client.setLed(true));
  const timeout = fakeConnection({ sleep: async () => {}, timeout: 10000 });
  await timeout.client.connect(); emit(timeout.device, 3, telemetry());
  timeout.device.receiveFeatureReport = async () => result(0, 0, 0);
  await assert.rejects(timeout.client.setLed(true), says(/uncertain/));
  await assert.rejects(timeout.client.setLed(true), says(/uncertain/));
  assert.equal(timeout.sent.length, 1);
  await timeout.client.disconnect();
  await client.disconnect();
});

test("hanging SET/GET requests time out as uncertain and never replay", async () => {
  for (const operation of ["sendFeatureReport", "receiveFeatureReport"]) {
    const { client, device, sent } = fakeConnection({ timeout: 10 });
    await client.connect(); emit(device, 3, telemetry());
    device[operation] = () => new Promise(() => {});
    await assert.rejects(client.setLed(true), says(/timeout/));
    assert.equal(client.state.outcome_uncertain, true);
    await assert.rejects(client.setLed(true), says(/uncertain/));
    assert.equal(sent.length, operation === "sendFeatureReport" ? 0 : 1);
    await client.disconnect();
  }
});

test("settings completion remains serialized and verifies saved values without a config read", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect();
  emit(device, 3, telemetry()); emit(device, 10, storage()); emit(device, 11, selfTest());
  const normalRead = device.receiveFeatureReport;
  let release;
  device.receiveFeatureReport = id => id === 20 ? new Promise(resolve => { release = resolve; }) : normalRead(id);
  const saving = client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.busy, true);
  await assert.rejects(client.setLed(true), says(/unavailable/));
  const incorrect = result(1, 5); incorrect[5] = 150; incorrect[7] = 1;
  release(incorrect);
  await assert.rejects(saving, says(/did not confirm/));
  assert.equal(sent.length, 1);
  assert.equal(client.state.settings.system_clock_mhz, 200);
  await client.disconnect();
});

test("normal and forced saves keep writes blocked when returned settings differ without disconnect", async () => {
  for (const flags of [0, 1]) {
    const { client, device, sent } = fakeConnection({ capabilities: 119 });
    await client.connect();
    const receive = device.receiveFeatureReport;
    device.receiveFeatureReport = async id => {
      const response = await receive(id);
      if (id === 20) response[5] = 150;
      return response;
    };
    await assert.rejects(client.command(5, { flags, args: [200, 0, 1] }), says(/did not confirm/));
    assert.equal(client.state.outcome_uncertain, true);
    await assert.rejects(client.command(1, { args: [1] }), says(/uncertain/));
    assert.equal(sent.length, 1);
    await client.disconnect();
  }
});

test("cancelling an in-flight write preserves uncertainty and its recovery journal", async () => {
  const { client, device, sent, pendingStore } = fakeConnection();
  let release;
  device.sendFeatureReport = async (id, report) => {
    sent.push([id, ...report]);
    await new Promise(resolve => { release = resolve; });
  };
  await client.connect();
  const controller = new AbortController();
  const command = client.command(OP.LED, { args: [1], signal: controller.signal });
  assert.equal(commandPending(client.state), true);
  controller.abort();
  await assert.rejects(command, { name: "AbortError" });
  assert.equal(client.state.outcome_uncertain, true);
  assert.ok(client.state.pending_request);
  assert.ok(pendingStore.getItem("picoforge.direct.pending-operation"));
  release();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sent.length, 1, "late completion never replays the write or clears its uncertainty");
  assert.equal(client.state.outcome_uncertain, true);
  await client.disconnect();
  client.dispose();
});

test("save commit-mask mismatch is uncertain even after firmware clears staged state", async () => {
  const setup = await connected(); radio(setup);
  const read = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => {
    const bytes = await read(id);
    if (id === 20 && bytes[2] === 5) bytes[9] = 0;
    return bytes;
  };
  await assert.rejects(setup.client.saveWireless({ ssid: "", password: "", wifi_enabled: false,
    ble_enabled: false, board: BOARD }), says(/did not confirm/));
  assert.equal(setup.client.state.outcome_uncertain, true);
  await assert.rejects(setup.client.command(1), says(/uncertain/));
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
  await assert.rejects(setup.client.scanWireless(), says(/generation and index/));
  assert.equal(setup.client.state.outcome_uncertain, true);
  const request = JSON.parse(setup.pendingStore.getItem("picoforge.direct.pending-operation")).request;
  assert.equal(request.scan_generation, 1); assert.equal(request.scan_index, 0);
  await setup.client.disconnect();
});
