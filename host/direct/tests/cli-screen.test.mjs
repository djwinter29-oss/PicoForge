import assert from "node:assert/strict";
import test from "node:test";

import { CliScreen, cliInputForKey } from "../js/ui/cli-screen.mjs";

test("CLI screen applies the Embedded CLI line-editing escapes", () => {
  const screen = new CliScreen();
  screen.write("> helo\x1b[2D\x1b[@l\r\n> x\x1b[s\tab\x1b[u\x1b[P");
  assert.deepEqual(screen.display(), ["> hello\n> x", " ", "   ab"]);
  screen.write("\x1b]junk\x1b[5Zok");
  assert.deepEqual(screen.display(), ["> hello\n> xjunkok", " ", ""], "non-CSI and unknown CSI escapes are dropped");
});

test("CLI screen bounds rows and columns", () => {
  const screen = new CliScreen();
  screen.write(`${"x".repeat(600)}\n`.repeat(1005));
  const [before, cursor, after] = screen.display();
  const rows = before.split("\n");
  assert.equal(rows.length, 1000);
  assert.equal(rows[0].length, 512);
  assert.deepEqual([cursor, after], [" ", ""]);
});

test("CLI keys map to the bytes the firmware expects", () => {
  const key = (name, modifiers = {}) => cliInputForKey({ key: name, ctrlKey: false, altKey: false,
    metaKey: false, ...modifiers });
  assert.equal(key("Enter"), "\r");
  assert.equal(key("ArrowLeft"), "\x1b[D");
  assert.equal(key("c", { ctrlKey: true }), "\x03");
  assert.equal(key("1", { ctrlKey: true }), null);
  assert.equal(key("v", { metaKey: true }), null);
  assert.equal(key("Shift"), null);
  assert.equal(key("é"), "é");
});
