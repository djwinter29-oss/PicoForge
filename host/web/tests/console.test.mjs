import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

test("console page retains public asset URLs and accessible terminal controls", () => {
  const html = readFileSync(new URL("../src/console.html", import.meta.url), "utf8");
  for (const asset of ["/css/dashboard.css", "/css/console.css", "/js/console.mjs"]) assert.ok(html.includes(asset));
  assert.doesNotMatch(html, /(?:src|href)="\/src\//);
  assert.match(html, /id="terminal"[^>]*tabindex="0"[^>]*role="textbox"/);
  assert.match(html, /aria-multiline="true" aria-readonly="true"/);
  assert.match(html, /id="console-status" role="status" aria-live="polite"/);
  assert.match(html, /Shift\+Tab leaves/);
});
