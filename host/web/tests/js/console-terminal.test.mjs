import assert from "node:assert/strict";
import test from "node:test";
import { ConsoleTerminal, consoleKey } from "../../src/js/console-terminal.mjs";

test("CLI terminal handles fragmented editing escapes, insert/delete and saved cursor during asynchronous output", () => {
  const terminal = new ConsoleTerminal();
  terminal.write("PicoForge> hl");
  terminal.write("\x1b[");
  terminal.write("D\x1b[@e\x1b[C");
  assert.equal(terminal.lines[0], "PicoForge> hel");
  terminal.write("\x1b[D\x1b[P");
  assert.equal(terminal.lines[0], "PicoForge> he");
  terminal.write("\x1b[s\r\nprogress\r\n");
  terminal.write("\x1b[u");
  terminal.write("lp");
  assert.equal(terminal.lines[0], "PicoForge> help");
  assert.equal(terminal.lines[1], "progress");
  assert.equal(terminal.display().join(""), "PicoForge> help \nprogress\n");
  terminal.write("\r<script>");
  assert.ok(terminal.lines[0].startsWith("<script>"));
});

test("CLI terminal bounds scrollback and line size and forwards terminal keys without local echo", () => {
  const terminal = new ConsoleTerminal();
  terminal.write("x".repeat(2000));
  assert.equal(terminal.lines[0].length, 512);
  terminal.write("\r\n".repeat(1200));
  assert.equal(terminal.lines.length, 1000);
  assert.equal(terminal.row, 999);
  for (const [key, expected] of [["Enter", "\r"], ["Tab", "\t"], ["Backspace", "\x7f"],
    ["ArrowUp", "\x1b[A"], ["ArrowLeft", "\x1b[D"], ["a", "a"]]) {
    assert.equal(consoleKey({ key }), expected);
  }
  assert.equal(consoleKey({ key: "c", ctrlKey: true }), "\x03");
  assert.equal(consoleKey({ key: "a", altKey: true }), null);
});

test("CLI help tabs align subcommands at eight-column stops, including fragmented output", () => {
  const terminal = new ConsoleTerminal();
  terminal.write(" * board\r\n");
  terminal.write("\tget <clock | led>\r\n\t");
  terminal.write("set led <on | off>\r\n * config\r\n\tset key=value");
  assert.deepEqual(terminal.lines, [
    " * board", "        get <clock | led>", "        set led <on | off>",
    " * config", "        set key=value",
  ]);
  terminal.write("\r12345678\tX");
  assert.equal(terminal.lines[4][16], "X");
  terminal.write("\r" + "\t".repeat(100) + "Z");
  assert.equal(terminal.column, 511);
  assert.equal(terminal.lines[4].length, 512);
});

