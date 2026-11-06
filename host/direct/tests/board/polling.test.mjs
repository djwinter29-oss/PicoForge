import assert from "node:assert/strict";
import test from "node:test";
import { mscPending, selfTestPending } from "../../web/js/state/selectors.mjs";
import { OP } from "../../web/js/protocol/constants.mjs";
import { describe } from "../../web/js/ui/messages.mjs";
import { BoardSession } from "../../web/js/board/session.mjs";
import {
  telemetry, storage, selfTest, result, fakeConnection, emit, attachDevice, says, patchState,
} from "../support/fixtures.mjs";
import { lifecycleOutcome, ReportReceipts, wirelessPollInterval } from "../../web/js/board/polling.mjs";
import { REASON } from "../../web/js/state/errors.mjs";
import { BleTransport } from "../../web/js/transport/ble.mjs";
import { status, connected } from "../support/wireless.mjs";

test("freshness is independent, exact at deadlines, and clears caches on disconnect/reconnect", async () => {
  let now = 0;
  const { client, device, hid } = fakeConnection({ now: () => now });
  await client.connect();
  assert.equal(client.state.freshness[3], "unavailable");
  emit(device, 3, telemetry()); emit(device, 10, storage()); emit(device, 11, selfTest());
  assert.ok(client.polling.receipts.timer, "fresh reports schedule the next expiry instead of polling");
  now = 749; client.expireReports();
  assert.ok(client.state.runtime);
  now = 750; client.expireReports();
  assert.equal(client.state.runtime, null);
  assert.equal(client.state.freshness[3], "stale");
  assert.ok(client.state.memory);
  now = 1499; client.expireReports();
  assert.ok(client.state.self_test);
  now = 1500; client.expireReports();
  assert.equal(client.state.memory, null);
  assert.equal(client.state.self_test, null);
  emit(device, 3, telemetry());
  assert.equal(client.state.freshness[3], "fresh");
  const invalid = telemetry(); invalid[0] = 2;
  now = 2000; emit(device, 3, invalid);
  now = 2250; client.expireReports();
  assert.equal(client.state.runtime, null, "invalid reports never refresh receipt time");
  emit(device, 3, telemetry()); client.expireReports(true);
  assert.equal(client.state.runtime, null);
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  assert.equal(client.polling.receipts.timer, null);
  for (const field of ["hardware", "settings", "runtime", "memory", "msc", "self_test"]) {
    assert.equal(client.state[field], null, field);
  }
  await attachDevice(client, device);
  assert.equal(client.state.freshness[10], "unavailable");
  assert.equal(client.state.memory, null);
  await client.disconnect();
});


test("USB sharing stays pending through delivery, old reports, reconnect and ownership transitions", async () => {
  const { client, device, hid } = fakeConnection();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  const send = device.sendFeatureReport;
  let release;
  device.sendFeatureReport = async (...args) => {
    await new Promise(resolve => { release = resolve; });
    return send(...args);
  };
  const sharing = client.setMsc(true);
  assert.equal(mscPending(client.state), true, "submission hides benchmark immediately");
  emit(device, 10, storage());
  assert.equal(mscPending(client.state), true);
  release();
  await sharing;
  emit(device, 10, storage());
  assert.equal(mscPending(client.state), true, "old firmware ownership cannot release latch");
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  device.opened = false;
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(mscPending(client.state), true, "reconnect cannot release latch");
  emit(device, 10, storage());
  assert.equal(mscPending(client.state), true);
  const host = storage(); host[2] = 1; host.fill(0, 3, 31); host[31] = 1; host[32] = 1;
  emit(device, 10, host);
  assert.equal(mscPending(client.state), true, "transition must finish");
  host[32] = 0; emit(device, 10, host);
  emit(device, 11, selfTest());
  assert.equal(mscPending(client.state), false);
  device.sendFeatureReport = send;
  await client.setMsc(false, true);
  emit(device, 10, host);
  assert.equal(mscPending(client.state), true, "return waits for firmware ownership");
  emit(device, 10, storage());
  assert.equal(mscPending(client.state), false);
  await client.disconnect();
});


test("rejected USB sharing releases pending state without inventing ownership", async () => {
  for (const commandRejected of [false, true]) {
    const { client, device } = fakeConnection();
    await client.connect();
    emit(device, 10, storage()); emit(device, 11, selfTest());
    const receive = device.receiveFeatureReport;
    if (commandRejected) device.receiveFeatureReport = id =>
      id === 20 ? result(1, 3, 5, 5) : receive(id);
    if (commandRejected) await assert.rejects(client.setMsc(true), says(/status 5/));
    else {
      await client.setMsc(true);
      const rejected = storage(); rejected[33] = 1;
      emit(device, 10, rejected);
    }
    assert.equal(mscPending(client.state), false);
    assert.equal(client.state.msc.enabled, false);
    await client.disconnect();
  }
});


test("self-test acceptance keeps controls pending until a newer report, including sequence rollover", async () => {
  const { client, device } = fakeConnection();
  await client.connect();
  emit(device, 10, storage());
  const idle = selfTest(); idle[1] = 254;
  emit(device, 11, idle);
  const send = device.sendFeatureReport;
  let release;
  device.sendFeatureReport = async (...args) => {
    await new Promise(resolve => { release = resolve; });
    return send(...args);
  };
  const starting = client.setSelfTest("start", 32);
  idle[1] = 255;
  emit(device, 11, idle);
  assert.equal(client.state.busy, true, "new telemetry cannot release controls during delivery");
  await assert.rejects(client.setSelfTest("start", 32), says(/unavailable/));
  release();
  await starting;
  emit(device, 11, idle);
  assert.equal(selfTestPending(client.state), true);
  await assert.rejects(client.setSelfTest("start", 32), says(/Fresh|unavailable/));
  const running = selfTest(1); running[1] = 0;
  emit(device, 11, running);
  assert.equal(selfTestPending(client.state), false);
  assert.equal(client.state.self_test.active, true);
  await client.disconnect();
});


test("Report12 freshness uses active500ms/idle2s and invalid reads never refresh the receipt", async () => {
  let now = 0;
  const setup = fakeConnection({ capabilities: 128 | 55, now: () => now });
  await setup.client.connect();
  let current = status();
  const read = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => id === 12 ? current : read(id);
  await setup.client.readWireless();
  now = 5999; setup.client.expireReports(); assert.ok(setup.client.state.wireless);
  now = 6000; setup.client.expireReports(); assert.equal(setup.client.state.wireless, null);
  current = status({ state: 4, flags: 48, testGeneration: 1 });
  await setup.client.readWireless();
  assert.equal(wirelessPollInterval(setup.client.state.wireless), 500);
  current = new Uint8Array(53);
  now = 7499; await assert.rejects(setup.client.readWireless());
  setup.client.expireReports(); assert.ok(setup.client.state.wireless);
  now = 7500; setup.client.expireReports(); assert.equal(setup.client.state.wireless, null);
  await setup.client.disconnect();
});


test("delayed BLE status reads preserve command matching and never replay a rejected GATT write", async () => {
  const setup = await connected();
  const transport = new BleTransport({});
  const client = new BoardSession(transport, { pendingStore: setup.pendingStore, wirelessPolling: false,
    now: () => 2000 });
  patchState(client, { ...setup.client.state, link: "ble" });
  setup.client.dispose();
  const device = Object.assign(new EventTarget(), { gatt: { connected: true, disconnect() {} } });
  transport.device = device;
  client.polling.onInputReport({ reportId: 3, data: telemetry() });
  const reports = { 3: telemetry(), 10: storage(), 11: selfTest(), 12: status() };
  let releaseTelemetry, releaseWireless;
  let telemetryPending = true, wirelessPending = true, rejectWrite = false;
  let wirelessReads = 0;
  const writes = [];
  for (const id of [3, 10, 11, 12, 20]) {
    transport.characteristics.set(id, {
      async readValue() {
        if (id === 3 && telemetryPending) {
          await new Promise(resolve => { releaseTelemetry = resolve; });
          telemetryPending = false;
        }
        if (id === 12) {
          wirelessReads++;
          if (wirelessPending) await new Promise(resolve => { releaseWireless = resolve; });
        }
        const request = writes.at(-1);
        const bytes = id === 20 ? result(request[1], request[2]).slice(0, 9) : reports[id];
        return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      },
      async writeValueWithResponse(request) {
        if (rejectWrite) throw new DOMException("GATT operation already in progress", "NetworkError");
        writes.push([...request]);
      },
    });
  }
  try {
    const poll = client.polling.pollReports();
    assert.equal(typeof releaseTelemetry, "function");
    await client.setLed(true);
    assert.equal(writes.length, 1, "an overlapping status read cannot consume a command result");
    rejectWrite = true;
    await assert.rejects(client.setLed(false), { code: REASON.BLE_PROOF_REJECTED });
    assert.equal(client.state.pending_request, null, "a native SET rejection is authoritative");
    assert.equal(client.state.outcome_uncertain, false);
    releaseTelemetry();
    await poll;
    assert.equal(client.state.freshness[3], "fresh");
    assert.equal(writes.length, 1, "the rejected write is not retried when the status read finishes");
    rejectWrite = false;
    await client.setLed(false);
    assert.equal(writes.length, 2, "only an explicit later action sends another write");
    client.polling.enabled = true;
    client.pollWireless();
    client.pollWireless();
    assert.equal(wirelessReads, 1, "Feature 12 has one in-flight background reader");
    client.connection.clear();
    wirelessPending = false;
    releaseWireless();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(client.state.wireless, null, "late BLE status cannot restore a disconnected snapshot");
    assert.equal(writes.length, 2);
  } finally { client.dispose(); }
});


test("lifecycleOutcome reads one Feature 12 snapshot against the accepted generation", () => {
  const scan = { operation: OP.SCAN, generation: 4 };
  assert.equal(lifecycleOutcome({ scan_generation: 4, scan_state: 1 }, scan), null);
  assert.equal(lifecycleOutcome({ scan_generation: 4, scan_state: 2 }, scan).kind, "completed");
  const failed = lifecycleOutcome({ scan_generation: 4, scan_state: 3 }, scan);
  assert.deepEqual([failed.kind, describe(failed.error)], ["failed", "Wireless scan failed"]);
  assert.throws(() => lifecycleOutcome({ scan_generation: 5, scan_state: 2 }, scan), says(/generation changed/));
  const wifiTest = { operation: OP.TEST_WIFI, generation: 2 };
  assert.equal(lifecycleOutcome({ test_generation: 2, state: 5, pair_tested: true }, wifiTest).result
    .remaining_seconds, 599);
  assert.ok(says(/^Password rejected$/)(lifecycleOutcome({ test_generation: 2, state: 6, detail: 3 }, wifiTest).error));
});


test("report receipts expire each snapshot after its own lifetime and on suspend", () => {
  let now = 0;
  const receipts = new ReportReceipts(globalThis, () => now, () => null);
  receipts.record(3);
  receipts.record(10);
  now = 800;
  assert.deepEqual(receipts.expire({ 3: "fresh", 10: "fresh" }),
    { runtime: null, freshness: { 3: "stale", 10: "fresh" } });
  assert.equal(receipts.expire({ 10: "fresh" }), null, "storage outlives telemetry");
  assert.equal(receipts.expire({ 10: "fresh" }, true).freshness[10], "stale", "suspend expires everything");
  assert.equal(receipts.size, 0);
});

