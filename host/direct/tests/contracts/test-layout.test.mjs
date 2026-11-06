// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import test from "node:test";

test("module test paths identify their source and helpers remain outside runnable suites", () => {
  const root = new URL("../", import.meta.url);
  const suites = readdirSync(root, { recursive: true }).filter(path => path.endsWith(".test.mjs"));
  assert.ok(suites.length > 0);
  for (const entry of suites) {
    const path = entry.replaceAll("\\", "/");
    assert.ok(!/^(support|browser|types)\//.test(path), `${path}: not a runtime suite`);
    if (/^(integration|contracts)\//.test(path)) continue;
    const source = path.startsWith("web/")
      ? new URL(`../web/${path.slice(4).replace(".test.mjs", ".html")}`, root)
      : new URL(`../web/js/${path.replace(".test.mjs", ".mjs")}`, root);
    assert.ok(existsSync(source), `${path}: no matching source at ${source.pathname}`);
  }
});
