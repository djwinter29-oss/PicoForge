// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { queryElement, setText } from "../../web/js/ui/dom.mjs";
import { createDocument } from "../support/dom.mjs";

test("shared DOM dispatch invokes additive listeners before awaiting and closes dialogs once", async () => {
  const { element, fire } = createDocument();
  const dialog = element("#dialog");
  let release;
  const calls = [];
  dialog.addEventListener("click", async () => {
    calls.push("first");
    await new Promise(resolve => { release = resolve; });
    calls.push("finished");
  });
  dialog.addEventListener("click", () => calls.push("second"));
  const pending = fire("#dialog", "click");
  assert.deepEqual(calls, ["first", "second"]);
  release();
  await pending;
  assert.deepEqual(calls, ["first", "second", "finished"]);
  let closes = 0;
  dialog.addEventListener("close", () => closes++);
  dialog.showModal();
  dialog.close();
  dialog.close();
  assert.equal(closes, 1);
});

test("text rendering only updates changed values", () => {
  const previous = globalThis.document;
  let text = "", writes = 0;
  const element = { namespaceURI: "http://www.w3.org/1999/xhtml", get textContent() { return text; },
    set textContent(value) { text = value; writes++; } };
  globalThis.document = { querySelector: () => element };
  try {
    setText({ value: "Ready" });
    setText({ value: "Ready" });
    assert.equal(writes, 1);
    setText({ value: 42 });
    setText({ value: "42" });
    assert.equal(text, "42");
    assert.equal(writes, 2);
    setText({ value: null });
    assert.equal(text, "");
    assert.equal(writes, 3);
  } finally { globalThis.document = previous; }
});

test("required DOM lookup rejects absent and non-HTML nodes and caches per document", () => {
  let reads = 0;
  const node = { namespaceURI: "http://www.w3.org/1999/xhtml" };
  const root = { querySelector() { reads++; return node; } };
  assert.equal(queryElement("#value", root), node);
  assert.equal(queryElement("#value", root), node);
  assert.equal(reads, 1);
  const other = { namespaceURI: "http://www.w3.org/1999/xhtml" };
  assert.equal(queryElement("#value", { querySelector: () => other }), other);
  assert.throws(() => queryElement("#missing", { querySelector: () => null }), /not found/);
  assert.throws(() => queryElement("#svg", {
    querySelector: () => ({ namespaceURI: "http://www.w3.org/2000/svg" }),
  }), /not HTML/);
  assert.throws(() => queryElement("input#missing,dialog#present", {
    querySelector: () => ({ namespaceURI: "http://www.w3.org/1999/xhtml", localName: "dialog" }),
  }), /wrong tag/);
});
