import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { relative } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { BleTransport } from "../web/js/transport/ble.mjs";
import { HidTransport } from "../web/js/transport/hid.mjs";

const root = new URL("../web/js/", import.meta.url);
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
    ui: ["ui", "state", "protocol", "board", "timers.mjs"],
  };
  assert.deepEqual(paths.filter(path => !path.includes("/")).sort(), ["app.mjs", "timers.mjs"]);
  for (const [path, imports] of dependencies) {
    const layer = path.split("/")[0];
    for (const target of imports) {
      assert.ok(sources.has(target), `${path}: unresolved import ${target}`);
      if (allowed[layer]) assert.ok(allowed[layer].includes(target.split("/")[0]), `${path} must not import ${target}`);
      if (layer === "ui" && target.startsWith("board/")) {
        assert.ok(target.startsWith("board/workflows/"), `${path} must not construct or depend on a board session`);
      }
    }
    if (["protocol", "state", "board"].includes(layer)) {
      assert.doesNotMatch(sources.get(path), /\b(document|window|navigator)\s*(?:\.|\[)/,
        `${path} must not access browser APIs`);
    }
  }
  assert.doesNotMatch(sources.get("app.mjs"), /querySelector|queryElement|\.command\(/);
  assert.doesNotMatch(sources.get("board/session.mjs"), /HidTransport|BleTransport|receiveFeatureReport|sendFeatureReport/);
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

test("one domain writer: workflows use the session context and the UI never writes domain state", () => {
  for (const [path, source] of sources) {
    if (path !== "app.mjs") {
      assert.doesNotMatch(source, /\b(navigator|isSecureContext)\b(?!\s*:)/,
        `${path}: app.mjs is the only global reader`);
    }
    if (path.startsWith("ui/")) assert.doesNotMatch(source, /\.update\(\s*\{/, `${path} must not write domain state`);
    if (path.startsWith("board/workflows/")) {
      assert.doesNotMatch(source, /client\.(update|pendingWirelessCommand|savedSsidHex|exclusiveWireless)\b/,
        `${path} must use only the workflow context`);
    }
    if (path.startsWith("transport/")) {
      assert.ok(!dependencies.get(path).includes("protocol/commands.mjs"), `${path} must stay a byte pipe`);
    }
  }
  assert.ok(!dependencies.get("board/session.mjs").some(target => target.startsWith("board/workflows/")),
    "the session must not import the workflows that drive it");
});

test("transports are byte pipes that honour abort, reject bad framing and wipe the control key", async () => {
  const aborted = AbortSignal.abort();
  const live = new AbortController().signal;
  const hid = new HidTransport(undefined);
  await assert.rejects(hid.readReport(10, aborted), { name: "AbortError" });
  await assert.rejects(hid.writeReport(20, new Uint8Array(68), live), /USB device disconnected/);
  const ble = new BleTransport(undefined);
  await assert.rejects(ble.writeReport(20, new Uint8Array(68), aborted), { name: "AbortError" });
  const oversized = new Uint8Array(68);
  oversized[9] = 1;
  await assert.rejects(ble.writeReport(20, oversized, live), /9-byte BLE control frame/);
  await assert.rejects(ble.writeReport(2, new Uint8Array(68), live), /9-byte BLE control frame/);
  await assert.rejects(ble.setControlKey(new Uint8Array(31)), /Control key must be 32 bytes/);
});
