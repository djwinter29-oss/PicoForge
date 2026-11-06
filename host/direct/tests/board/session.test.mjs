import assert from "node:assert/strict";
import test from "node:test";
import { OP } from "../../web/js/protocol/constants.mjs";
import { BoardSession } from "../../web/js/board/session.mjs";
import { identity, config, telemetry, result, fakeConnection, emit } from "../support/fixtures.mjs";

test("a disposed client ignores HID events and stops its expiry timer", async () => {
  const { client, device, hid } = fakeConnection();
  await client.connect();
  emit(device, 3, telemetry());
  const receipt = client.polling.receipts.get(3);
  client.dispose();
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  emit(device, 3, telemetry());
  assert.equal(client.state.connected, true, "a replaced client must not react to the shared navigator.hid");
  assert.equal(client.polling.receipts.get(3), receipt, "input reports no longer reach a disposed client");
});


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
    const session = new BoardSession(transport,
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

