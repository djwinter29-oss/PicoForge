import assert from "node:assert/strict";
import test from "node:test";
import { describe } from "../../web/js/ui/messages.mjs";
import { identity, config, selfTest, fakeConnection, emit } from "../support/fixtures.mjs";

test("mandatory connection reads reject malformed identity/config failures and gate self-test input", async () => {
  const { client, device, reads } = fakeConnection({ capabilities: 1 | 2 | 4 | 16 });
  await client.connect();
  assert.deepEqual(reads, [1, 2]);
  assert.equal(client.state.connected, true);
  emit(device, 11, selfTest());
  assert.equal(client.state.self_test, null);
  await client.disconnect();
  const old = fakeConnection();
  old.device.receiveFeatureReport = async () => new DataView(new Uint8Array(42).buffer);
  await old.client.connect();
  assert.equal(old.client.state.connected, false);
  assert.match(describe(old.client.state.error), /exactly 26/);
  const missing = fakeConnection();
  missing.device.receiveFeatureReport = async id => {
    if (id === 1) return identity();
    throw new Error("missing config");
  };
  await missing.client.connect();
  assert.equal(missing.client.state.connected, false);
  assert.match(describe(missing.client.state.error), /missing config/);
});

test("MSC-style re-enumeration reconnects by Feature 1 UID, not a non-standard serialNumber property", async () => {
  const { client, device, hid } = fakeConnection();
  assert.equal("serialNumber" in device, false);
  await client.connect();
  const uid = client.state.hardware.serial_number;
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  device.opened = false;
  assert.equal(client.state.reconnecting, true);

  const receive = device.receiveFeatureReport;
  const otherIdentity = identity();
  otherIdentity[3] ^= 0xff;
  device.receiveFeatureReport = id => id === 1 ? otherIdentity : receive(id);
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, false, "another PicoForge device must not be attached");
  assert.equal(client.state.reconnecting, true);

  device.receiveFeatureReport = receive;
  device.opened = false;
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, true);
  assert.equal(client.state.hardware.serial_number, uid);
  await client.disconnect();
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
  client.connection.reconnectUid = "0102030405060708";
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
