// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { createHmac } from "node:crypto";
import { BleTransport } from "../../web/js/transport/ble.mjs";
import { says } from "../support/fixtures.mjs";
import { BOARD, connected, radio } from "../support/wireless.mjs";

test("all-zero32byte keys remain configured; only empty keys clear", async () => {
  for (const key of ["0".repeat(64), ""]) {
    const setup = await connected(); radio(setup);
    try {
      const saved = await setup.client.saveWireless({ ssid: "home", wifi_enabled: true, ble_enabled: true,
        ble_key: key, board: { ...BOARD, radios: 3 } });
      assert.equal(saved.ble_key_configured, key !== "");
      assert.deepEqual(setup.sent.map(bytes => bytes[3]), [8, 5]);
      assert.equal(setup.sent[0][5], key === "" ? 0 : 32, "presence is determined by length, not byte values");
      assert.ok(setup.sent[0].slice(6).every(byte => byte === 0));
    } finally { await setup.client.disconnect(); }
  }
  const client = new BleTransport(undefined);
  await client.setControlKey(new Uint8Array(32));
  assert.ok(client.bleKey, "an all-zero key can authorize a Bluetooth session");
  const message = Uint8Array.of(1, 2, 3);
  assert.deepEqual(Buffer.from(await crypto.subtle.sign("HMAC", client.bleKey, message)),
    createHmac("sha256", Buffer.alloc(32)).update(message).digest());
  const short = new Uint8Array(31).fill(1);
  await assert.rejects(client.setControlKey(short), says(/32 bytes/));
  assert.ok(short.every(byte => byte === 0), "the transport wipes the caller's key bytes");
});
