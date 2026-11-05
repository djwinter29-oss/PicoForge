import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import { readFileSync } from "node:fs";
import test from "node:test";

test("Direct console uses the connected dashboard for authentication without asking for the key", async () => {
  const handlers = new Map();
  const windowHandlers = new Map();
  const elements = new Map();
  const channels = [];
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      disabled: false, hidden: false, value: "", textContent: "",
      addEventListener: (type, handler) => handlers.set(`${selector}:${type}`, handler),
      closest() { return elements.get(`${selector}:label`) ?? elements.set(`${selector}:label`, { hidden: false })
        .get(`${selector}:label`); },
      setAttribute() {}, replaceChildren() {}, focus() {},
    });
    return elements.get(selector);
  };
  const saved = { document: globalThis.document, window: globalThis.window, location: globalThis.location,
    fetch: globalThis.fetch };
  const savedCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  const calls = [];
  let readSignal;
  class FakeBroadcastChannel {
    constructor(name) { this.name = name; channels.push(this); }
    postMessage(message) {
      this.request = message;
      queueMicrotask(() => this.onmessage?.({ data: { type: "tcp-cli-auth-response",
        auth_token: message.auth_token, request_id: message.request_id, uid: message.uid,
        proof: "ab".repeat(16) } }));
    }
    close() { this.closed = true; }
  }
  globalThis.document = { querySelector: element, createElement: () => ({}), createTextNode: text => ({ text }) };
  globalThis.location = { search: "?ipv4=192.168.66.87&auth_token=0123456789abcdef0123456789abcdef" };
  globalThis.window = { addEventListener: (type, handler) => windowHandlers.set(type, handler),
    BroadcastChannel: FakeBroadcastChannel, getSelection: () => ({ toString: () => "" }), confirm: () => true };
  Object.defineProperty(globalThis, "crypto", { configurable: true, value: webcrypto });
  globalThis.fetch = async (url, options) => {
    const operation = url.split("/").at(-1);
    calls.push([operation, JSON.parse(options.body)]);
    if (operation === "open") return { ok: true, json: async () => ({ session: "session-a",
      challenge: { nonce: "000102030405060708090a0b0c0d0e0f", uid: "0102030405060708" } }) };
    if (operation === "read") {
      readSignal = options.signal;
      return new Promise((_, reject) => {
        options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
      });
    }
    return { ok: true, json: async () => ({}) };
  };
  const tick = () => new Promise(resolve => setImmediate(resolve));
  try {
    await import(`../js/console.mjs?test=${Date.now()}`);
    assert.equal(element("#console-target").textContent, "192.168.66.87:2323");
    assert.equal(elements.has("#console-auth-key"), false);
    assert.doesNotMatch(readFileSync(new URL("../console.html", import.meta.url), "utf8"), /console-auth-key/);
    handlers.get("#connect:click")();
    await tick();
    assert.deepEqual(calls[0], ["open", { ipv4: "192.168.66.87" }]);
    assert.deepEqual(channels[0].request, { type: "tcp-cli-auth-request",
      auth_token: "0123456789abcdef0123456789abcdef", request_id: channels[0].request.request_id,
      nonce: "000102030405060708090a0b0c0d0e0f", uid: "0102030405060708" });
    assert.match(channels[0].request.request_id, /^[a-f0-9]{32}$/);
    assert.equal(channels[0].closed, true);
    assert.deepEqual(calls[1], ["auth", { session: "session-a", proof: "ab".repeat(16) }]);
    assert.equal(element("#console-status").textContent, "Connected");
    assert.equal(element("#connect").textContent, "Disconnect", "one button toggles while connected");

    handlers.get("#terminal:keydown")({ key: "Tab", preventDefault() {}, isComposing: false,
      ctrlKey: false, altKey: false, metaKey: false, shiftKey: false });
    await tick();
    const write = calls.find(([operation]) => operation === "write");
    assert.deepEqual(write[1], { session: "session-a", data: btoa("\t") }, "Tab is forwarded for completion");

    handlers.get("#connect:click")();
    await tick();
    await tick();
    assert.equal(readSignal.aborted, true);
    assert.deepEqual(calls.at(-1), ["close", { session: "session-a" }]);
    assert.equal(element("#console-status").textContent, "Disconnected");
    assert.equal(element("#connect").textContent, "Connect");
    assert.equal(element("#connect").disabled, false);
  } finally {
    Object.assign(globalThis, saved);
    if (savedCrypto) Object.defineProperty(globalThis, "crypto", savedCrypto);
    else delete globalThis.crypto;
  }
});
