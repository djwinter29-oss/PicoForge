import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

test("shared pages use the same favicon as Direct", () => {
  const shared = readFileSync(new URL("../src/favicon.svg", import.meta.url));
  assert.deepEqual(shared, readFileSync(new URL("../../direct/web/favicon.svg", import.meta.url)));
  for (const page of ["../src/index.html", "../src/console.html"]) {
    assert.match(readFileSync(new URL(page, import.meta.url), "utf8"), /\/favicon.svg\?v=3/);
  }
});
