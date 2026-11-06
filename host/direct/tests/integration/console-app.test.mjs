// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { webcrypto } from "node:crypto";
import { readFileSync } from "node:fs";
import { mountConsole } from "../../web/js/ui/console.mjs";
import { createDocument } from "../support/dom.mjs";

test("console coalesces output frames, sends input immediately and never replays a partial write", async () => {
  const { document, element, fire } = createDocument();
  document.createTextNode = data => ({ data });
  const handlers = new Map();
  const frames = new Map();
  const calls = [];
  let frameId = 0, reads = 0, readSignal;
  const opener = { postMessage(message) {
    queueMicrotask(() => handlers.get("message")?.({
      origin: "http://localhost", source: opener,
      data: { type: "direct-console-auth-response", request_id: message.request_id,
        uid: message.uid, proof: "ab".repeat(16) },
    }));
  } };
  const window = {
    opener, addEventListener: (type, handler) => handlers.set(type, handler),
    removeEventListener: type => handlers.delete(type),
    requestAnimationFrame: callback => { frames.set(++frameId, callback); return frameId; },
    cancelAnimationFrame: id => frames.delete(id),
  };
  const fetch = async (url, options) => {
    const operation = url.split("/").at(-1);
    calls.push(operation);
    let result;
    if (operation === "open") result = { session: "session-a",
      challenge: { nonce: "ab".repeat(16), uid: "cd".repeat(8) } };
    else if (operation === "auth") result = { authenticated: true };
    else if (operation === "write") result = { written: 1 }; // A two-byte write must fail without replay.
    else if (operation === "read") {
      reads++;
      if (reads <= 2) result = { data: btoa(reads === 1 ? "first" : "second"), closed: false };
      else {
        readSignal = options.signal;
        return new Promise((_, reject) => options.signal.addEventListener("abort",
          () => reject(options.signal.reason), { once: true }));
      }
    } else result = { closed: true };
    return { ok: true, json: async () => result };
  };
  const tick = () => new Promise(resolve => setTimeout(resolve, 10));
  const wait = async condition => {
    const deadline = Date.now() + 2000;
    while (!condition() && Date.now() < deadline) await tick();
    assert.equal(Boolean(condition()), true, "console continuation did not settle");
  };
  mountConsole({ document, window, location: { search: "?ipv4=192.168.1.9", origin: "http://localhost" }, fetch });
  try {
    await fire("#connect", "click");
    await wait(() => reads === 3);
    assert.equal(frameId, 1, "two output chunks share one deferred frame");
    assert.equal(frames.size, 1);
    const terminal = element("#terminal");
    assert.equal(terminal.children[0].data, "", "output remains pending until the frame");
    terminal.dispatchEvent(Object.assign(new Event("keydown", { cancelable: true }), {
      key: "Enter", isComposing: false, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false,
    }));
    assert.equal(calls.filter(operation => operation === "write").length, 1, "input was not delayed by output");
    await tick();
    const paste = Object.assign(new Event("paste", { cancelable: true }), {
      clipboardData: { getData: () => "ab" },
    });
    terminal.dispatchEvent(paste);
    await wait(() => element("#console-status").textContent.includes("did not confirm all input bytes"));
    assert.match(element("#console-status").textContent, /input was not resent/);
    assert.equal(calls.filter(operation => operation === "write").length, 2, "partial input was not replayed");
    assert.equal(terminal.children[0].data, "firstsecond", "disconnect flushes final output");
    assert.equal(frames.size, 0, "disconnect cancels the stale output frame");
    assert.equal(readSignal.aborted, true);
    reads = 0;
    await fire("#connect", "click");
    await wait(() => reads === 3);
    const callback = frames.values().next().value;
    frames.clear();
    callback();
    assert.equal(terminal.children[0].data, "firstsecond", "the shared frame renders both chunks");
    await fire("#connect", "click");
    reads = 0;
    await fire("#connect", "click");
    await wait(() => reads === 3);
    assert.equal(frames.size, 1);
    handlers.get("pagehide")();
    assert.equal(frames.size, 0, "navigation cancels the pending output frame");
    assert.equal(readSignal.aborted, true);
    assert.equal(terminal.children[0].data, "", "navigation does not render stale buffered output");
  } finally { handlers.get("pagehide")(); }
});

test("malformed successful bridge responses surface errors without continuing or retrying", async () => {
  for (const malformedOperation of ["open", "auth", "read"]) {
    const { document, element, fire } = createDocument();
    document.createTextNode = data => ({ data });
    const handlers = new Map();
    const calls = [];
    const opener = { postMessage(message) {
      queueMicrotask(() => handlers.get("message")?.({
        origin: "http://localhost", source: opener,
        data: { type: "direct-console-auth-response", request_id: message.request_id,
          uid: message.uid, proof: "ab".repeat(16) },
      }));
    } };
    const window = {
      opener, addEventListener: (type, handler) => handlers.set(type, handler),
      removeEventListener: type => handlers.delete(type),
      requestAnimationFrame() { assert.fail("Invalid output must not schedule a render"); },
      cancelAnimationFrame() {},
    };
    const fetch = async url => {
      const operation = url.split("/").at(-1);
      calls.push(operation);
      const responses = {
        open: { session: "session-a", challenge: { nonce: "ab".repeat(16), uid: "cd".repeat(8) } },
        auth: { authenticated: true }, close: { closed: true },
      };
      return { ok: true, json: async () => operation === malformedOperation ? {} : responses[operation] };
    };
    mountConsole({ document, window, location: { search: "?ipv4=192.168.1.9", origin: "http://localhost" }, fetch });
    try {
      await fire("#connect", "click");
      await new Promise(resolve => setImmediate(resolve));
      assert.match(element("#console-status").textContent,
        new RegExp(`Invalid Direct terminal bridge ${malformedOperation} response`));
      assert.equal(element("#connect").disabled, false);
      assert.equal(calls.filter(operation => operation === malformedOperation).length, 1);
      if (malformedOperation !== "open") assert.equal(calls.at(-1), "close");
      assert.equal(calls.includes("write"), false);
    } finally { handlers.get("pagehide")(); }
  }
});

test("Direct console uses the connected dashboard for authentication without asking for the key", async () => {
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
  const savedCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  const calls = [];
  let reloads = 0;
  let readSignal;
  globalThis.document = { querySelector: element, createElement: () => ({}), createTextNode: text => ({ text }) };
  globalThis.location = { search: "?ipv4=192.168.66.87", origin: "http://localhost",
    href: "http://localhost/console.html?ipv4=192.168.66.87", reload() { reloads++; } };
  const dashboardWindow = {
    postMessage(message) {
      this.request = message;
      queueMicrotask(() => windowHandlers.get("message")?.({ origin: "http://localhost", source: dashboardWindow,
        data: { type: "direct-console-auth-response", request_id: message.request_id, uid: message.uid,
          proof: "ab".repeat(16) } }));
    },
  };
  globalThis.window = { opener: dashboardWindow,
    addEventListener: (type, handler) => windowHandlers.set(type, handler),
    removeEventListener: type => windowHandlers.delete(type),
    getSelection: () => ({ toString: () => "" }), confirm: () => true,
    requestAnimationFrame: callback => setTimeout(callback, 0), cancelAnimationFrame: clearTimeout };
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
    const result = operation === "auth" ? { authenticated: true }
      : operation === "write" ? { written: atob(JSON.parse(options.body).data).length } : { closed: true };
    return { ok: true, json: async () => result };
  };
  const tick = () => new Promise(resolve => setImmediate(resolve));
  try {
    await import(`../../web/js/console-app.mjs?test=${Date.now()}`);
    windowHandlers.get("pageshow")({ persisted: false });
    assert.equal(reloads, 0, "initial page display must not reload");
    assert.equal(element("#console-target").textContent, "192.168.66.87:2323");
    assert.equal(elements.has("#console-auth-key"), false);
    assert.doesNotMatch(readFileSync(new URL("../../web/console.html", import.meta.url), "utf8"), /console-auth-key/);
    handlers.get("#connect:click")();
    await tick();
    assert.deepEqual(calls[0], ["open", { ipv4: "192.168.66.87" }]);
    assert.deepEqual(dashboardWindow.request, { type: "direct-console-auth-request",
      request_id: dashboardWindow.request.request_id,
      nonce: "000102030405060708090a0b0c0d0e0f", uid: "0102030405060708" });
    assert.match(dashboardWindow.request.request_id, /^[a-f0-9]{32}$/);
    assert.equal(globalThis.window.opener, dashboardWindow, "only the original dashboard authorizes reconnections");
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
    handlers.get("#connect:click")();
    await tick();
    assert.equal(element("#console-status").textContent, "Connected", "Connect works again in the same tab");
    assert.equal(calls.filter(([operation]) => operation === "auth").length, 2,
      "each connection obtains a fresh board challenge proof");
    windowHandlers.get("pagehide")({ persisted: true });
    assert.equal(readSignal.aborted, true, "cached console pages abort pending reads");
    assert.deepEqual(calls.at(-1), ["close", { session: "session-a" }]);
    windowHandlers.get("pageshow")({ persisted: true });
    assert.equal(reloads, 1, "history-cache restoration loads a fresh disconnected console");
    assert.equal(calls.filter(([operation]) => operation === "open").length, 2,
      "restoration must not open a session or resend input");
  } finally {
    Object.assign(globalThis, saved);
    if (savedCrypto) Object.defineProperty(globalThis, "crypto", savedCrypto);
    else delete globalThis.crypto;
  }
});
