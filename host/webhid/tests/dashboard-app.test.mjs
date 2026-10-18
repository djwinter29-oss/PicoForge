import assert from "node:assert/strict";
import test from "node:test";

test("shared dashboard handlers survive a status failure that clears the snapshot mid-command", async () => {
  const handlers = new Map();
  let tick;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) {
      const target = { textContent: "", valueAsNumber: 32, dataset: {}, style: {}, classList: { toggle() {}, add() {}, remove() {} },
        addEventListener: (type, handler) => handlers.set(`${selector}:${type}`, handler) };
      elements.set(selector, new Proxy(target, { get: (object, key) => key in object ? object[key] : () => {} }));
    }
    return elements.get(selector);
  };
  const connected = { connected: true, error: null, board_status: { led_available: true, led_on: false },
    msc: { enabled: false, transition_pending: false }, self_test: null };
  let statusFails = false;
  let release;
  let gate;
  const saved = { document: globalThis.document, window: globalThis.window, fetch: globalThis.fetch };
  globalThis.document = { querySelector: element };
  globalThis.window = { setInterval: callback => { tick = callback; }, confirm: () => true };
  globalThis.fetch = async url => {
    if (url === "/api/status") {
      if (statusFails) throw new Error("status down");
      return { ok: true, json: async () => structuredClone(connected) };
    }
    await gate;
    return { ok: false, status: 503, json: async () => ({ error: "HID command delivery failed" }) };
  };
  try {
    await import("../../web/js/app.mjs");
    await new Promise(resolve => setImmediate(resolve));
    for (const name of ["#led-control:click", "#msc-control:click", "#self-test-start:click"]) {
      gate = new Promise(resolve => { release = resolve; });
      statusFails = false;
      tick();
      await new Promise(resolve => setImmediate(resolve));
      const pending = handlers.get(name)?.();
      statusFails = true;
      tick();
      await new Promise(resolve => setImmediate(resolve));
      release();
      await assert.doesNotReject(async () => pending, name);
    }
  } finally {
    Object.assign(globalThis, saved);
  }
});
