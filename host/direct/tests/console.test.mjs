import assert from "node:assert/strict";
import test from "node:test";

test("Direct console relays keystrokes through the local bridge and closes the session on disconnect", async () => {
  const handlers = new Map();
  const windowHandlers = new Map();
  const elements = new Map();
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
  const calls = [];
  let readSignal;
  globalThis.document = { querySelector: element, createElement: () => ({}), createTextNode: text => ({ text }) };
  globalThis.location = { search: "?ipv4=192.168.66.87&auth=1" };
  globalThis.window = { addEventListener: (type, handler) => windowHandlers.set(type, handler),
    getSelection: () => ({ toString: () => "" }), confirm: () => true };
  globalThis.fetch = async (url, options) => {
    const operation = url.split("/").at(-1);
    calls.push([operation, JSON.parse(options.body)]);
    if (operation === "open") return { ok: true, json: async () => ({ session: "session-a" }) };
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
    assert.equal(element("#console-auth-key").closest().hidden, false, "auth=1 shows the key field");
    await handlers.get("#connect:click")();
    assert.equal(calls.length, 0, "a required key is checked before contacting the bridge");
    assert.match(element("#console-status").textContent, /control key/);

    element("#console-auth-key").value = "ab".repeat(32);
    handlers.get("#connect:click")();
    await tick();
    assert.deepEqual(calls[0], ["open", { ipv4: "192.168.66.87", auth_required: true, ble_key: "ab".repeat(32) }]);
    assert.equal(element("#console-auth-key").value, "", "the key is cleared after the request");
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
  }
});
