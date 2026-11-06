// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

test("dashboard styles expose wireless selection and focus and wrap dialog actions", () => {
  const css = readFileSync(new URL("../../src/css/dashboard.css", import.meta.url), "utf8");
  assert.match(css, /\.wifi-network\[aria-pressed="true"\]/);
  assert.match(css, /\.wifi-network:focus-visible/);
  assert.match(css, /\.dialog-actions \{[^}]*flex-wrap: wrap/);
});
