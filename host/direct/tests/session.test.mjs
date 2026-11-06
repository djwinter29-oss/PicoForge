import assert from "node:assert/strict";
import test from "node:test";
import { commandPending } from "../web/js/state/selectors.mjs";
import { BoardSession } from "../web/js/board/session.mjs";
import { OP } from "../web/js/protocol/constants.mjs";
import { config, fakeConnection, identity, result, telemetry } from "./support/fixtures.mjs";

test("the same board session consumes either peer transport without HID-shaped device APIs", async () => {
  for (const kind of ["usb", "ble"]) {
    const sent = [];
    const transport = Object.assign(new EventTarget(), {
      kind, available: true, autoReconnect: false, pollInterval: null, device: null,
      async connect(_options, signal) { signal.throwIfAborted(); this.device = {}; return true; },
      async disconnect() { this.device = null; },
      async dispose() { await this.disconnect(); },
      async writeReport(id, data, signal) { signal.throwIfAborted(); sent.push([id, [...data]]); },
      async readReport(id, signal) {
        signal.throwIfAborted();
        if (id === 1) return identity();
        if (id === 2) return config();
        return result(sent.at(-1)[1][1], sent.at(-1)[1][2]);
      },
    });
    const session = new BoardSession(transport, () => {},
      { pendingStore: fakeConnection().pendingStore, wirelessPolling: false });
    await session.connect();
    transport.dispatchEvent(Object.assign(new Event("report"), { reportId: 3, data: telemetry() }));
    await session.setLed(true);
    assert.equal(session.state.link, kind);
    assert.equal(session.state.freshness[3], "fresh");
    assert.equal(sent.length, 1);
    assert.equal(sent[0][1][2], OP.LED);
    await session.disconnect();
    session.dispose();
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
  assert.ok(client.outstanding);
  assert.ok(pendingStore.getItem("picoforge.direct.pending-operation"));
  release();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sent.length, 1, "late completion never replays the write or clears its uncertainty");
  assert.equal(client.state.outcome_uncertain, true);
  await client.disconnect();
  client.dispose();
});

test("an aborted initialization cannot close or overwrite a newer same-board connection", async () => {
  const { client, device, hid } = fakeConnection();
  const receive = device.receiveFeatureReport;
  let release;
  device.receiveFeatureReport = id => id === 1
    ? new Promise(resolve => { release = resolve; }) : receive(id);
  const opening = client.connect();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(typeof release, "function");
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  device.opened = false;
  client.reconnectUid = "0102030405060708";
  device.receiveFeatureReport = receive;
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, true);
  release(identity());
  await opening;
  assert.equal(client.state.connected, true);
  assert.equal(device.opened, true);
  assert.equal(client.state.error, null);
  await client.disconnect();
  client.dispose();
});
