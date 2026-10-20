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
  const requestedUrls = [];
  const saved = { document: globalThis.document, window: globalThis.window, fetch: globalThis.fetch };
  globalThis.document = {
    querySelector: element,
    createElement: () => ({ value: "", textContent: "" }),
  };
  globalThis.window = { setInterval: callback => { tick = callback; }, confirm: () => true };
  globalThis.fetch = async url => {
    requestedUrls.push(url);
    if (url === "/api/boards") {
      return { ok: true, json: async () => ({
        boards: [
          { id: "board-a", product_name: "PicoForge", serial_number: "board-A", device_path: "/hid/a" },
          { id: "board-b", product_name: "PicoForge", serial_number: "board-B", device_path: "/hid/b" },
        ],
        default_board_id: "board-a",
      }) };
    }
    if (url.startsWith("/api/status?board=")) {
      if (statusFails) throw new Error("status down");
      return { ok: true, json: async () => structuredClone(connected) };
    }
    await gate;
    return { ok: false, status: 503, json: async () => ({ error: "HID command delivery failed" }) };
  };
  try {
    await import("../../web/js/app.mjs");
    await new Promise(resolve => setImmediate(resolve));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(elements.get("#board-select").value, "board-a");
    assert.ok(requestedUrls.includes("/api/status?board=board-a"));
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
    assert.ok(requestedUrls.includes("/api/led?board=board-a"));
    elements.get("#board-select").value = "board-b";
    handlers.get("#board-select:change")();
    await new Promise(resolve => setImmediate(resolve));
    assert.ok(requestedUrls.includes("/api/status?board=board-b"));
  } finally {
    Object.assign(globalThis, saved);
  }
});
