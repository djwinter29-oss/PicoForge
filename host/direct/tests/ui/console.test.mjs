// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { validateBridgeResponse } from "../../web/js/ui/console.mjs";

test("console bridge validates each operation and refuses success-shaped malformed responses", () => {
  const valid = {
    open: { session: "session-a", challenge: { nonce: "ab".repeat(16), uid: "cd".repeat(8) } },
    auth: { authenticated: true }, read: { data: btoa("hello"), closed: false },
    write: { written: 1024 }, close: { closed: true },
  };
  for (const [operation, value] of Object.entries(valid)) {
    assert.deepEqual(validateBridgeResponse(operation, value), value);
    for (const malformed of [null, [], {}, "OK"]) {
      assert.throws(() => validateBridgeResponse(operation, malformed), /Invalid Direct terminal bridge/);
    }
  }
  for (const value of [
    { ...valid.open, session: "" }, { ...valid.open, challenge: { nonce: "invalid", uid: "cd".repeat(8) } },
    { ...valid.open, challenge: { nonce: "ab".repeat(16), uid: 123 } },
  ]) assert.throws(() => validateBridgeResponse("open", value), /open response/);
  for (const value of [{ data: "!", closed: false }, { data: "", closed: "false" }, { data: "abc", closed: true }]) {
    assert.throws(() => validateBridgeResponse("read", value), /read response/);
  }
  assert.deepEqual(validateBridgeResponse("read", { data: "", closed: true }), { data: "", closed: true });
  for (const written of [0, -1, 4097, 1.5, "1"]) {
    assert.throws(() => validateBridgeResponse("write", { written }), /write response/);
  }
  assert.throws(() => validateBridgeResponse("auth", { authenticated: false }), /auth response/);
  assert.throws(() => validateBridgeResponse("close", { closed: false }), /close response/);
});
