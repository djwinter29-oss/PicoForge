import assert from "node:assert/strict";
import test from "node:test";
import { setText } from "../../web/js/ui/dom.mjs";

test("text rendering only updates changed values", () => {
  const previous = globalThis.document;
  let text = "", writes = 0;
  const element = { get textContent() { return text; },
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

