// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { readdirSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = new URL("./", import.meta.url);
const files = readdirSync(root, { recursive: true }).filter(path => path.endsWith(".test.mjs")).sort()
  .map(path => fileURLToPath(new URL(path.replaceAll("\\", "/"), root)));
if (!files.length) throw new Error("No Direct JavaScript tests found");

const result = spawnSync(process.execPath, ["--test", ...process.argv.slice(2), ...files], { stdio: "inherit" });
if (result.error) throw result.error;
if (result.signal) throw new Error(`Direct tests terminated by ${result.signal}`);
process.exitCode = result.status;
