import assert from "node:assert/strict";
import test from "node:test";
import { readdirSync, readFileSync } from "node:fs";
import { relative } from "node:path";
import { fileURLToPath } from "node:url";
import { BleTransport } from "../../web/js/transport/ble.mjs";
import { HidTransport } from "../../web/js/transport/hid.mjs";
import { canTransition, PHASE } from "../../web/js/board/connection.mjs";
import { OP } from "../../web/js/protocol/constants.mjs";
import { OPCODES } from "../../web/js/protocol/opcodes.mjs";
import { DOMAIN_EVENTS } from "../../web/js/state/reducer.mjs";
import { BoardError, REASON } from "../../web/js/state/errors.mjs";
import { describe, MESSAGES } from "../../web/js/ui/messages.mjs";
import { says } from "../support/fixtures.mjs";

const root = new URL("../../web/js/", import.meta.url);
const paths = readdirSync(root, { recursive: true }).filter(path => path.endsWith(".mjs"))
  .map(path => path.replaceAll("\\", "/"));
const sources = new Map(paths.map(path => [path, readFileSync(new URL(path, root), "utf8")]));
const dependencies = new Map(paths.map(path => [path, [...sources.get(path).matchAll(/\bfrom\s+["'](\.[^"']+)["']/g)]
  .map(([, specifier]) => relative(fileURLToPath(root), fileURLToPath(new URL(specifier, new URL(path, root))))
    .replaceAll("\\", "/"))]));

test("Direct modules keep the agreed ownership and dependency boundaries", () => {
  const allowed = {
    protocol: ["protocol"], transport: ["transport", "protocol", "timers.mjs"],
    state: ["state", "protocol"], board: ["board", "protocol", "state", "timers.mjs"],
    ui: ["ui", "state", "protocol", "timers.mjs"],
  };
  assert.deepEqual(paths.filter(path => !path.includes("/")).sort(), ["app.mjs", "console-app.mjs", "timers.mjs"]);
  // The reason-code vocabulary is a leaf every layer may import, so no layer needs English text to report a failure.
  assert.deepEqual(dependencies.get("state/errors.mjs"), [], "state/errors.mjs imports nothing");
  for (const [path, imports] of dependencies) {
    const layer = path.split("/")[0];
    for (const target of imports) {
      assert.ok(sources.has(target), `${path}: unresolved import ${target}`);
      if (allowed[layer] && target !== "state/errors.mjs") {
        assert.ok(allowed[layer].includes(target.split("/")[0]), `${path} must not import ${target}`);
      }
    }
    if (["protocol", "state", "board"].includes(layer)) {
      assert.doesNotMatch(sources.get(path), /\b(document|window|navigator)\s*(?:\.|\[)/,
        `${path} must not access browser APIs`);
    }
  }
  assert.doesNotMatch(sources.get("app.mjs"), /querySelector|queryElement|\.command\(/);
  assert.doesNotMatch(sources.get("board/session.mjs"), /HidTransport|BleTransport|receiveFeatureReport|sendFeatureReport/);
  assert.doesNotMatch(sources.get("ui/sections/wireless-form.mjs"), /getRandomValues|writeText\(|bytesToHex/,
    "wireless coordination delegates key generation and clipboard work");
  assert.doesNotMatch(sources.get("ui/sections/wireless-form-view.mjs"),
    /addEventListener|setInterval|setTimeout|\.scanWireless\(|\.testWireless\(|\.cancelWireless\(/,
    "wireless presentation applies snapshots without initiating operations");
});

test("the Direct runtime module graph is acyclic", () => {
  const visited = new Set();
  function visit(path, stack = []) {
    assert.ok(!stack.includes(path), `Import cycle: ${[...stack, path].join(" -> ")}`);
    if (visited.has(path)) return;
    for (const target of dependencies.get(path)) visit(target, [...stack, path]);
    visited.add(path);
  }
  for (const path of paths) visit(path);
});

test("one domain writer: board modules emit named events, the connection owns its phase, the UI only renders", () => {
  for (const [path, source] of sources) {
    if (!["app.mjs", "console-app.mjs"].includes(path)) {
      assert.doesNotMatch(source, /\b(navigator|isSecureContext)\b(?!\s*:)/,
        `${path}: the composition roots are the only global readers`);
    }
    if (path.startsWith("ui/")) {
      assert.doesNotMatch(source, /\.emit\(|\b(client|session)\.(update\(|store\.dispatch)/,
        `${path} must not write domain state`);
      // The console window shows only WebSocket/browser errors; messages.mjs is where codes become text.
      if (!["ui/console.mjs", "ui/messages.mjs"].includes(path)) {
        assert.doesNotMatch(source, /\.message\b/, `${path} must render errors through describe()`);
      }
    }
    if (path !== "board/connection.mjs") {
      assert.doesNotMatch(source, /emit\(\s*["']connection\//, `${path}: only the connection emits connection events`);
    }
    if (path.startsWith("board/") && path !== "board/session.mjs") {
      assert.ok(!dependencies.get(path).includes("board/session.mjs"), `${path} must not import the facade`);
    }
    if (path.startsWith("transport/")) {
      const pipe = target => target === "state/errors.mjs" || !/^(board|state|ui)\//.test(target);
      assert.ok(dependencies.get(path).every(pipe),
        `${path} must stay a byte pipe`);
    }
  }
  for (const [path, source] of sources) {
    for (const [, event] of source.matchAll(/emit\(\s*["']([^"']+)["']/g)) {
      assert.ok(Object.hasOwn(DOMAIN_EVENTS, event), `${path}: undeclared domain event ${event}`);
    }
  }
  const facade = sources.get("board/session.mjs").replace(/\/\*[\s\S]*?\*\//g, "");
  assert.ok(facade.split("\n").filter(line => line.trim()).length <= 100, "the session stays a thin facade");
});

test("user-visible text lives only in ui/messages.mjs: lower layers report reason codes", () => {
  // Programmer invariants, never shown in normal operation, stay plain and surface as UNEXPECTED.
  const invariants =
    /Illegal connection transition|Timer scope is disposed|Unknown (state action|domain event|\$\{label\} keys)/;
  for (const [path, source] of sources) {
    if (!/^(protocol|transport|board|state)\/|^timers\.mjs$/.test(path)) continue;
    for (const [literal] of source.matchAll(/new (?:Error|DOMException|BoardError)\(\s*["'`][^"'`]*/g)) {
      assert.match(literal, invariants, `${path}: throw a reason code, not text: ${literal}`);
    }
    assert.doesNotMatch(source, /onProgress\?\.\(\s*["'`]/, `${path}: progress is a reason code`);
  }
});

test("the connection lifecycle allows only the declared transitions", () => {
  const forward = [PHASE.DISCONNECTED, PHASE.CONNECTING, PHASE.INITIALIZING, PHASE.RECOVERING, PHASE.READY];
  for (let i = 1; i < forward.length; i++) assert.ok(canTransition(forward[i - 1], forward[i]), forward[i]);
  assert.ok(canTransition(PHASE.RECONNECTING, PHASE.INITIALIZING));
  for (const phase of Object.values(PHASE)) {
    assert.ok(canTransition(phase, PHASE.DISCONNECTED) && canTransition(phase, PHASE.RECONNECTING), phase);
  }
  assert.ok(!canTransition(PHASE.DISCONNECTED, PHASE.READY), "ready only after recovery");
  assert.ok(!canTransition(PHASE.CONNECTING, PHASE.RECOVERING), "recovery only after initialization");
  assert.ok(!canTransition(PHASE.READY, PHASE.INITIALIZING), "a live link must close before reinitializing");
});

test("every opcode has one complete descriptor and every reason code has dashboard text", () => {
  assert.deepEqual(Object.keys(OPCODES).map(Number).sort((a, b) => a - b),
    Object.values(OP).sort((a, b) => a - b), "one descriptor per opcode");
  for (const [opcode, descriptor] of Object.entries(OPCODES)) {
    assert.equal(typeof descriptor.capability, "number", `${opcode} capability`);
    assert.ok(descriptor.ble === null || typeof descriptor.ble === "object", `${opcode} BLE policy`);
    assert.equal(typeof descriptor.request, "function", `${opcode} request rule`);
  }
  for (const code of Object.values(REASON)) {
    assert.ok(Object.hasOwn(MESSAGES, code), `no text for ${code}`);
    assert.notEqual(describe(new BoardError(code, {})), code, code);
  }
  assert.equal(describe(new DOMException("Timed out", "TimeoutError")), "Unexpected error: Timed out",
    "DOMException numeric codes are not reasons; uncoded errors keep their message only as diagnostic detail");
  assert.match(describe(new DOMException(REASON.IO_TIMEOUT, "TimeoutError")), /timeout; command outcome uncertain/);
  assert.equal(describe(new BoardError(REASON.INVALID_REPORT, "self-test-status")), "Invalid HID self-test status");
  assert.equal(describe(new BoardError(REASON.INVALID_INPUT, "control-key")), "Invalid Control key (64 hex digits)");
  assert.equal(describe(Object.assign(new Error("x"), { name: "NotFoundError" })), "No board was selected.");
});

test("transports are byte pipes that honour abort, reject bad framing and wipe the control key", async () => {
  const aborted = AbortSignal.abort();
  const live = new AbortController().signal;
  const hid = new HidTransport(undefined);
  assert.equal(await hid.createConsoleProof(new Uint8Array(16), new Uint8Array(8), live), null,
    "USB proof generation stays on the serialized board command path");
  await assert.rejects(hid.readReport(10, aborted), { name: "AbortError" });
  await assert.rejects(hid.writeReport(20, new Uint8Array(68), live), says(/USB device disconnected/));
  const ble = new BleTransport(undefined);
  await assert.rejects(ble.writeReport(20, new Uint8Array(68), aborted), { name: "AbortError" });
  const oversized = new Uint8Array(68);
  oversized[9] = 1;
  await assert.rejects(ble.writeReport(20, oversized, live), says(/9-byte BLE control frame/));
  await assert.rejects(ble.writeReport(2, new Uint8Array(68), live), says(/9-byte BLE control frame/));
  await assert.rejects(ble.setControlKey(new Uint8Array(31)), says(/Control key must be 32 bytes/));
});
