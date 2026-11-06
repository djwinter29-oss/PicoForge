import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { relative } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = new URL("../js/", import.meta.url);
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
