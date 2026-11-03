import assert from "node:assert/strict";
import test from "node:test";
import { bindDirectTerminal } from "../js/terminal.mjs";

test("Direct terminal sends the board address and required key only to the local bridge", async () => {
  const previous = { fetch: globalThis.fetch, setTimeout: globalThis.setTimeout,
    clearTimeout: globalThis.clearTimeout };
  const elements = new Map();
  let bridgeAvailable = true;
  for (const id of ["dialog", "connect", "disconnect", "close", "status", "output", "input", "key", "key-label"]) {
    elements.set(id, { value: "", textContent: "", disabled: false, hidden: false, open: false, scrollTop: 0,
      scrollHeight: 0, handlers: {}, addEventListener(name, callback) { this.handlers[name] = callback; },
      showModal() { this.open = true; }, close() { this.open = false; this.handlers.close?.(); }, focus() {} });
  }
  const calls = [];
  globalThis.fetch = async (url, request) => {
    const payload = JSON.parse(request.body);
    calls.push({ url, payload });
    return { ok: true, status: 200, json: async () => url.endsWith("/health")
      ? { available: bridgeAvailable } : url.endsWith("/open") ? { session: "session-token" }
        : url.endsWith("/read") ? { data: "", closed: false } : { closed: true } };
  };
  globalThis.setTimeout = () => 1;
  globalThis.clearTimeout = () => {};
  const el = id => elements.get(id);
  try {
    const terminal = bindDirectTerminal({ dialog: el("dialog"), connect: el("connect"), disconnect: el("disconnect"),
      close: el("close"), status: el("status"), output: el("output"), input: el("input"), key: el("key"),
      keyLabel: el("key-label"), getWireless: () => ({ wifi_enabled: true, wifi_state: "connected",
        tcp_cli_started: true, tcp_cli_auth_required: true, ipv4: "192.168.1.9" }) });
    await terminal.open();
    assert.equal(el("dialog").open, true);
    assert.equal(el("key-label").hidden, false);
    el("key").value = "ab".repeat(32);
    await el("connect").handlers.click();
    assert.deepEqual(calls[0], { url: "/api/direct-terminal/health", payload: {} });
    assert.deepEqual(calls[1], { url: "/api/direct-terminal/open", payload: {
      ipv4: "192.168.1.9", auth_required: true, ble_key: "ab".repeat(32),
    } });
    assert.equal(el("key").value, "", "the key input is cleared after the request");
    assert.equal(el("status").textContent, "Connected to 192.168.1.9:2323.");
    el("disconnect").handlers.click();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(calls.at(-1).url, "/api/direct-terminal/close");
    assert.equal(el("disconnect").disabled, true);
    el("key").value = "ab".repeat(32);
    el("dialog").close();
    assert.equal(el("key").value, "", "closing the terminal clears the authentication key");
    bridgeAvailable = false;
    await terminal.open();
    assert.equal(calls.at(-1).url, "/api/direct-terminal/health");
    assert.deepEqual(calls.at(-1).payload, {}, "bridge discovery never includes authentication credentials");
    assert.equal(el("key-label").hidden, true);
    assert.equal(el("connect").disabled, true);
    assert.match(el("status").textContent, /tools\/host\/run-direct\.sh from a checkout/);
    assert.match(el("status").textContent, /python3 server\.py from the release ZIP/);
  } finally {
    globalThis.fetch = previous.fetch;
    globalThis.setTimeout = previous.setTimeout;
    globalThis.clearTimeout = previous.clearTimeout;
  }
});
