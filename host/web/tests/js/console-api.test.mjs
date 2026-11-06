import assert from "node:assert/strict";
import test from "node:test";
import { consoleRequest, validateConsoleResponse } from "../../src/js/console-api.mjs";

test("console success bodies enforce each operation's backend contract", async context => {
  const invalid = [
    ["POST", {}], ["POST", { session: "" }], ["POST", { session: 1 }],
    ["GET", { data: "%%%", closed: false }], ["GET", { data: "eA==", closed: "false" }],
    ["PUT", {}], ["PUT", { ok: false }], ["DELETE", { ok: false }],
  ];
  for (const [method, result] of invalid) {
    assert.throws(() => validateConsoleResponse(result, method), /Invalid console/);
  }
  assert.deepEqual(validateConsoleResponse({ session: "token" }, "POST"), { session: "token" });
  assert.deepEqual(validateConsoleResponse({ data: "", closed: true }, "GET"), { data: "", closed: true });
  for (const method of ["PUT", "DELETE"]) {
    assert.deepEqual(validateConsoleResponse({ ok: true }, method), { ok: true });
  }
  context.mock.method(globalThis, "fetch", async () => ({ ok: true, json: async () => ({ ok: false }) }));
  await assert.rejects(consoleRequest("/api/console", "PUT", { data: "eA==" }), /Invalid console PUT/);
});

test("console requests enforce deadlines and abort with their session", async context => {
  let observedSignal;
  context.mock.method(globalThis, "fetch", (_url, options) => {
    observedSignal = options.signal;
    return new Promise((_, reject) => {
      options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
    });
  });
  await assert.rejects(consoleRequest("/api/console", "GET", undefined, undefined, 5),
    error => error.name === "TimeoutError");
  assert.equal(observedSignal.aborted, true);

  const session = new AbortController();
  const pending = consoleRequest("/api/console", "PUT", { data: "eA==" }, session.signal, 1000);
  session.abort();
  await assert.rejects(pending, error => error.name === "AbortError");
});

