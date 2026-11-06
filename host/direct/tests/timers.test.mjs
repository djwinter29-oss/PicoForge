// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { createTimerScope, wait, withTimeout } from "../web/js/timers.mjs";

test("page timer scopes cancel child work when an owner is disposed", async () => {
  const page = createTimerScope();
  const connection = page.createChild();
  let calls = 0;
  connection.setTimeout(() => calls++, 5);
  const interval = connection.setInterval(() => calls++, 2);
  connection.clearInterval(interval);
  connection.dispose();
  await new Promise(resolve => setTimeout(resolve, 15));
  assert.equal(calls, 0);
  assert.throws(() => connection.setTimeout(() => {}, 1), /disposed/);
  page.dispose();
});

test("timeouts and sleeps abort promptly with the owning connection signal", async () => {
  const controller = new AbortController();
  const pending = new Promise(() => {});
  const timeout = withTimeout(pending, 1000, controller.signal);
  const sleep = wait(1000, controller.signal);
  controller.abort(new DOMException("Connection changed", "AbortError"));
  await assert.rejects(timeout, { name: "AbortError", message: "Connection changed" });
  await assert.rejects(sleep, { name: "AbortError", message: "Connection changed" });
});
