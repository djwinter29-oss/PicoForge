// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { domFixture } from "../support/dom-fixture.mjs";

test("invalid console acknowledgements close the session without replaying input", async context => {
  const fixture = domFixture();
  const saved = { document: globalThis.document, window: globalThis.window, location: globalThis.location };
  Object.assign(globalThis, {
    document: fixture.document, location: { search: "?board=board-a" },
    window: { addEventListener() {}, getSelection: () => ({ toString: () => "" }), confirm: () => true },
  });
  context.after(() => Object.assign(globalThis, saved));
  const writes = [];
  context.mock.method(globalThis, "fetch", async (_url, options) => {
    if (options.method === "POST") return { ok: true, json: async () => ({ session: "token" }) };
    if (options.method === "DELETE") return { ok: true, json: async () => ({ ok: true }) };
    if (options.method === "PUT") {
      writes.push(JSON.parse(options.body));
      return { ok: true, json: async () => ({ ok: false }) };
    }
    return new Promise((_, reject) => options.signal.addEventListener("abort", () => reject(options.signal.reason)));
  });
  await import(`../../src/js/console.mjs?invalid-ack=${Date.now()}`);
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  await fixture.el("#terminal").fire("keydown", { key: "x" });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(writes.length, 1);
  assert.match(fixture.el("#console-status").textContent, /Invalid console PUT.*input was not resent/);
  assert.equal(fixture.el("#connect").textContent, "Connect");
});

test("console disconnect aborts a hung poll and does not retry pending input", async () => {
  const fixture = domFixture();
  const { handlers, elements } = fixture;
  const windowHandlers = new Map();
  const saved = {
    document: globalThis.document,
    window: globalThis.window,
    location: globalThis.location,
    fetch: globalThis.fetch,
  };
  let pollSignal;
  let inputRequests = 0;
  let connectSignal;
  let holdConnect = false;
  globalThis.document = fixture.document;
  globalThis.location = { search: "" };
  globalThis.window = {
    addEventListener: (type, handler) => windowHandlers.set(type, handler),
    getSelection: () => ({ toString: () => "" }), confirm: () => true,
  };
  globalThis.fetch = async (url, options = {}) => {
    if (options.method === "POST") {
      if (holdConnect) {
        connectSignal = options.signal;
        return new Promise((_, reject) => {
          options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
        });
      }
      return { ok: true, json: async () => ({ session: "session-a" }) };
    }
    if (options.method === "DELETE") return { ok: true, json: async () => ({ ok: true }) };
    if (options.method === "PUT") inputRequests++;
    pollSignal ??= options.signal;
    return new Promise((_, reject) => {
      options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
    });
  };
  try {
    await import(`../../src/js/console.mjs?disconnect-test=${Date.now()}`);
    handlers.get("#connect:click")();
    await new Promise(resolve => setImmediate(resolve));
    handlers.get("#terminal:keydown")({
      key: "x", preventDefault() {}, isComposing: false, ctrlKey: false, altKey: false, metaKey: false,
    });
    await new Promise(resolve => setImmediate(resolve));
    handlers.get("#connect:click")();
    await new Promise(resolve => setImmediate(resolve));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(pollSignal.aborted, true);
    assert.equal(inputRequests, 1);
    assert.equal(elements.get("#console-status").textContent, "Disconnected");
    holdConnect = true;
    handlers.get("#connect:click")();
    await new Promise(resolve => setImmediate(resolve));
    windowHandlers.get("pagehide")();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(connectSignal.aborted, true, "page teardown aborts a pending session creation request");
  } finally {
    globalThis.document = saved.document;
    globalThis.window = saved.window;
    globalThis.location = saved.location;
    globalThis.fetch = saved.fetch;
  }
});

test("console coalesces output frames, sends input immediately, flushes close, and cancels navigation output", async context => {
  const fixture = domFixture();
  const frames = new Map();
  const events = new Map();
  let nextFrame = 0, renders = 0, writes = 0;
  const terminal = fixture.el("#terminal");
  const { ConsoleTerminal } = await import("../../src/js/console-terminal.mjs");
  const cursorLine = ConsoleTerminal.prototype.cursorLine;
  context.mock.method(ConsoleTerminal.prototype, "cursorLine", function () { renders++; return cursorLine.call(this); });
  const saved = { document: globalThis.document, window: globalThis.window, location: globalThis.location };
  Object.assign(globalThis, { document: fixture.document, location: { search: "" }, window: {
    addEventListener: (name, callback) => events.set(name, callback),
    requestAnimationFrame: callback => { frames.set(++nextFrame, callback); return nextFrame; },
    cancelAnimationFrame: id => frames.delete(id),
    getSelection: () => ({ toString: () => "" }),
  } });
  context.after(() => Object.assign(globalThis, saved));
  let output = [];
  context.mock.method(globalThis, "fetch", async (_url, options) => {
    const body = result => ({ ok: true, json: async () => result });
    if (options.method === "POST") return body({ session: `token-${nextFrame}` });
    if (options.method === "DELETE") return body({ ok: true });
    if (options.method === "PUT") { writes++; return body({ ok: true }); }
    if (output.length) return body({ data: btoa(output.shift()), closed: false });
    return new Promise((_, reject) => options.signal.addEventListener("abort", () => reject(options.signal.reason)));
  });
  await import(`../../src/js/console.mjs?frames=${Date.now()}`);
  output = ["a", "b"];
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(frames.size, 1);
  assert.equal(renders, 1, "only the initial empty screen renders before the frame");
  await terminal.fire("keydown", { key: "x" });
  assert.equal(writes, 1, "input does not wait for the output frame");
  const [id, callback] = [...frames][0];
  frames.delete(id);
  callback();
  assert.equal(renders, 2);
  assert.match(terminal.children.map(child => child.textContent).join(""), /^ab/);
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  output = ["final"];
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(frames.size, 1);
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(frames.size, 0);
  assert.match(terminal.children.map(child => child.textContent).join(""), /^final/);
  output = ["navigation"];
  await fixture.el("#connect").fire("click");
  await new Promise(resolve => setImmediate(resolve));
  const before = renders;
  events.get("pagehide")();
  assert.equal(frames.size, 0);
  assert.equal(renders, before, "navigation cancels without painting stale output");
});

test("console preserves reading position and reuses unchanged rows while following output only at the bottom",
  async context => {
    const fixture = domFixture();
    const frames = new Map();
    let nextFrame = 0, pendingRead;
    const saved = { document: globalThis.document, window: globalThis.window, location: globalThis.location };
    Object.assign(globalThis, { document: fixture.document, location: { search: "" }, window: {
      addEventListener() {},
      requestAnimationFrame: callback => { frames.set(++nextFrame, callback); return nextFrame; },
      cancelAnimationFrame: id => frames.delete(id),
    } });
    context.after(() => Object.assign(globalThis, saved));
    context.mock.method(globalThis, "fetch", async (_url, options) => {
      const body = value => ({ ok: true, json: async () => value });
      if (options.method === "POST") return body({ session: "scroll-session" });
      if (options.method === "DELETE") return body({ ok: true });
      return new Promise((resolve, reject) => {
        pendingRead = text => resolve(body({ data: btoa(text), closed: false }));
        options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
      });
    });
    await import(`../../src/js/console.mjs?scroll=${Date.now()}`);
    await fixture.el("#connect").fire("click");
    await new Promise(resolve => setImmediate(resolve));
    const terminal = fixture.el("#terminal");
    const feed = async text => {
      pendingRead(text);
      await new Promise(resolve => setImmediate(resolve));
      for (const [id, callback] of frames) { frames.delete(id); callback(); }
    };
    await feed(("line\r\n").repeat(999));
    assert.equal(terminal.children.length, 1000);
    assert.equal(terminal.children[0].tagName, "DIV", "semantic blocks preserve native copied line separators");
    terminal.scrollHeight = 20000;
    terminal.clientHeight = 400;
    terminal.scrollTop = 5000;
    const retained = terminal.children[1];
    await feed("next\r\n");
    assert.equal(terminal.scrollTop, 4980, "dropping one row preserves the viewed content, not the bottom");
    assert.equal(terminal.children[0], retained);
    assert.equal(terminal.children.length, 1000);
    terminal.scrollTop = 19600;
    await feed("tail");
    assert.equal(terminal.scrollTop, terminal.scrollHeight, "bottom-follow remains available");
    const last = terminal.children.at(-1);
    await feed("\rX");
    assert.equal(terminal.children.at(-1), last, "cursor edits reuse the existing row");
    assert.match(last.textContent, /^Xail/);
    await fixture.el("#connect").fire("click");
    await new Promise(resolve => setImmediate(resolve));
  });
