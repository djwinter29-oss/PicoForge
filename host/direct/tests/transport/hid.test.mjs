import assert from "node:assert/strict";
import test from "node:test";
import { matchesPicoForge } from "../../web/js/protocol/codec.mjs";
import { describe } from "../../web/js/ui/messages.mjs";
import { fakeConnection } from "../support/fixtures.mjs";

test("explicit connect always requests selection regardless of previously authorized devices", async () => {
  for (const count of [0, 1, 2]) {
    const { client, hid, device } = fakeConnection();
    const other = fakeConnection().device;
    let selections = 0;
    hid.getDevices = async () => [other, device].slice(0, count);
    hid.requestDevice = async options => {
      selections++;
      assert.deepEqual(options.filters, [0x4011, 0x4012].map(productId =>
        ({ vendorId: 0xcafe, usagePage: 0xff00, usage: 1, productId })));
      return [device];
    };
    await client.connect();
    assert.equal(selections, 1);
    assert.equal(client.state.connected, true);
    assert.equal(client.device, device);
    assert.equal(other.opened, false);
    await client.disconnect();
  }
});

test("cancelled or denied selection never opens a previously authorized device", async () => {
  for (const denied of [false, true]) {
    const { client, hid, device } = fakeConnection();
    hid.requestDevice = async () => {
      if (denied) throw new Error("Device permission denied");
      return [];
    };
    await client.connect();
    assert.equal(client.state.connected, false);
    assert.equal(client.state.busy, false);
    assert.equal(device.opened, false);
    assert.equal(describe(client.state.error), denied ? "Unexpected error: Device permission denied" : null);
  }
});

test("development HID filter uses authorized vendor collection and both USB configurations", () => {
  const { device } = fakeConnection();
  assert.equal(matchesPicoForge(device), true);
  device.productId = 0x4012; assert.equal(matchesPicoForge(device), true);
  device.collections[0].usage = 2; assert.equal(matchesPicoForge(device), false);
});
