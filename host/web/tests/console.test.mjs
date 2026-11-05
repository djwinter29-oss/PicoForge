import assert from "node:assert/strict";
import test from "node:test";
import { ConsoleTerminal, consoleKey } from "../js/console-terminal.mjs";
import { consoleRequest } from "../js/console-api.mjs";
import { renderWirelessStatus } from "../js/wireless-form.mjs";

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

test("console disconnect aborts a hung poll and does not retry pending input", async () => {
  const handlers = new Map();
  const windowHandlers = new Map();
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      disabled: false,
      addEventListener: (type, handler) => handlers.set(`${selector}:${type}`, handler),
      setAttribute() {},
      replaceChildren() {},
      focus() {},
    });
    return elements.get(selector);
  };
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
  globalThis.document = {
    querySelector: element,
    createElement: () => ({}),
    createTextNode: text => ({ text }),
  };
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
    if (options.method === "DELETE") return { ok: true, json: async () => ({}) };
    if (options.method === "PUT") inputRequests++;
    pollSignal ??= options.signal;
    return new Promise((_, reject) => {
      options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
    });
  };
  try {
    await import(`../js/console.mjs?disconnect-test=${Date.now()}`);
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

test("CLI terminal handles fragmented editing escapes, insert/delete and saved cursor during asynchronous output", () => {
  const terminal = new ConsoleTerminal();
  terminal.write("PicoForge> hl");
  terminal.write("\x1b[");
  terminal.write("D\x1b[@e\x1b[C");
  assert.equal(terminal.lines[0], "PicoForge> hel");
  terminal.write("\x1b[D\x1b[P");
  assert.equal(terminal.lines[0], "PicoForge> he");
  terminal.write("\x1b[s\r\nprogress\r\n");
  terminal.write("\x1b[u");
  terminal.write("lp");
  assert.equal(terminal.lines[0], "PicoForge> help");
  assert.equal(terminal.lines[1], "progress");
  assert.equal(terminal.display().join(""), "PicoForge> help \nprogress\n");
  terminal.write("\r<script>");
  assert.ok(terminal.lines[0].startsWith("<script>"));
});

test("CLI terminal bounds scrollback and line size and forwards terminal keys without local echo", () => {
  const terminal = new ConsoleTerminal();
  terminal.write("x".repeat(2000));
  assert.equal(terminal.lines[0].length, 512);
  terminal.write("\r\n".repeat(1200));
  assert.equal(terminal.lines.length, 1000);
  assert.equal(terminal.row, 999);
  for (const [key, expected] of [["Enter", "\r"], ["Tab", "\t"], ["Backspace", "\x7f"],
    ["ArrowUp", "\x1b[A"], ["ArrowLeft", "\x1b[D"], ["a", "a"]]) {
    assert.equal(consoleKey({ key }), expected);
  }
  assert.equal(consoleKey({ key: "c", ctrlKey: true }), "\x03");
  assert.equal(consoleKey({ key: "a", altKey: true }), null);
});

test("CLI help tabs align subcommands at eight-column stops, including fragmented output", () => {
  const terminal = new ConsoleTerminal();
  terminal.write(" * board\r\n");
  terminal.write("\tget <clock | led>\r\n\t");
  terminal.write("set led <on | off>\r\n * config\r\n\tset key=value");
  assert.deepEqual(terminal.lines, [
    " * board", "        get <clock | led>", "        set led <on | off>",
    " * config", "        set key=value",
  ]);
  terminal.write("\r12345678\tX");
  assert.equal(terminal.lines[4][16], "X");
  terminal.write("\r" + "\t".repeat(100) + "Z");
  assert.equal(terminal.column, 511);
  assert.equal(terminal.lines[4].length, 512);
});

test("Wi-Fi label links only with a connected board, a started listener and a local console URL", () => {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      children: [],
      set textContent(value) { this.text = value; this.children = []; },
      get textContent() { return this.text; },
      querySelector() { return this.children[0]; },
      replaceChildren(...children) { this.children = children; },
    });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({}) };
  try {
    const state = { connected: true, hardware: { capabilities: 128 }, console_url: "/console?board=board-a",
      wireless: { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
        ssid: "HomeDev", ipv4: "192.168.66.87" } };
    renderWirelessStatus(state);
    const link = element("#wifi-state").children[0];
    assert.equal(link.textContent, "HomeDev (192.168.66.87)");
    assert.equal(link.href, "/console?board=board-a");
    assert.equal(link.target, "_blank");
    assert.equal(link.rel, "noopener noreferrer");
    renderWirelessStatus(state);
    assert.equal(element("#wifi-state").children[0], link, "polling preserves the focused link");
    for (const change of [{ connected: false }, { wireless_error: "read failed" }, { console_url: undefined },
      { wireless: { ...state.wireless, tcp_cli_started: false } },
      { wireless: { ...state.wireless, wifi_state: "connecting" } }]) {
      renderWirelessStatus({ ...state, ...change });
      assert.equal(element("#wifi-state").children.length, 0);
    }
  } finally { globalThis.document = saved; }
});
