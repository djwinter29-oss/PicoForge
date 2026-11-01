/**
 * CLI terminal: the Embedded CLI cursor/insert/delete escapes, with bounded plain-text scrollback.
 * ponytail: 1000 lines of 512 columns and no general VT100/color support suffice for this CLI;
 * use a full terminal emulator if commands begin producing other terminal protocols.
 */
export class ConsoleTerminal {
  lines = [""];
  row = 0;
  column = 0;
  saved = [0, 0];
  escape = "";

  write(text) {
    for (const character of text) {
      if (this.escape) {
        this.escape += character;
        if (this.escape === "\x1b[") continue;
        if (/^\x1b\[[0-9;]*[@-~]$/.test(this.escape)) {
          this.control(this.escape.slice(2));
          this.escape = "";
        } else if (!/^\x1b\[[0-9;]*$/.test(this.escape) || this.escape.length > 24) this.escape = "";
        continue;
      }
      if (character === "\x1b") { this.escape = character; continue; }
      if (character === "\r") this.column = 0;
      else if (character === "\n") {
        this.row++;
        this.column = 0;
        if (!this.lines[this.row]) this.lines[this.row] = "";
      } else if (character === "\b") this.column = Math.max(0, this.column - 1);
      else if (character === "\t") this.column = Math.min(511, (Math.floor(this.column / 8) + 1) * 8);
      else if (character >= " " && character !== "\x7f") {
        const line = this.lines[this.row].padEnd(this.column, " ");
        this.lines[this.row] = (line.slice(0, this.column) + character + line.slice(this.column + 1)).slice(0, 512);
        this.column = Math.min(511, this.column + 1);
      }
      if (this.lines.length > 1000) {
        this.lines.shift();
        this.row--;
        this.saved[0] = Math.max(0, this.saved[0] - 1);
      }
    }
  }

  control(sequence) {
    const command = sequence.at(-1);
    const count = Math.min(512, Number(sequence.slice(0, -1)) || 1);
    const line = this.lines[this.row];
    if (command === "C") this.column = Math.min(511, this.column + count);
    else if (command === "D") this.column = Math.max(0, this.column - count);
    else if (command === "s") this.saved = [this.row, this.column];
    else if (command === "u") [this.row, this.column] = this.saved;
    else if (command === "@") {
      this.lines[this.row] = (line.slice(0, this.column) + " ".repeat(count) + line.slice(this.column)).slice(0, 512);
    } else if (command === "P") {
      this.lines[this.row] = line.slice(0, this.column) + line.slice(this.column + count);
    }
  }

  /** Return text surrounding the cursor for safe text-node rendering (never HTML). */
  display() {
    const lines = [...this.lines];
    lines[this.row] = lines[this.row].padEnd(this.column + 1, " ");
    const offset = lines.slice(0, this.row).reduce((size, line) => size + line.length + 1, 0) + this.column;
    const text = lines.join("\n");
    return [text.slice(0, offset), text[offset], text.slice(offset + 1)];
  }
}

export function consoleKey(event) {
  if (event.ctrlKey && !event.altKey && event.key.length === 1) {
    const code = event.key.toUpperCase().charCodeAt(0);
    return code >= 64 && code <= 95 ? String.fromCharCode(code - 64) : null;
  }
  if (event.ctrlKey || event.altKey || event.metaKey) return null;
  return ({
    Enter: "\r", Tab: "\t", Backspace: "\x7f", Delete: "\x1b[3~",
    ArrowUp: "\x1b[A", ArrowDown: "\x1b[B", ArrowRight: "\x1b[C", ArrowLeft: "\x1b[D",
    Home: "\x1b[H", End: "\x1b[F", Escape: "\x1b",
  })[event.key] ?? (event.key.length === 1 ? event.key : null);
}
