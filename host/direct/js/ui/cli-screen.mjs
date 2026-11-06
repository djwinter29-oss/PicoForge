/**
 * @file Direct-owned screen model for the firmware Embedded CLI relayed over the TCP console.
 * The firmware line editor emits only CR, LF, BS, TAB and the CSI finals C/D (cursor), s/u (save/restore),
 * @ (insert blanks) and P (delete); every other escape is consumed and dropped.
 * ponytail: plain-text rows capped at MAX_ROWS x MAX_COLUMNS with no colours or full VT100; adopt a terminal
 * emulator if the firmware starts emitting other terminal protocols.
 */

const MAX_ROWS = 1000;
const MAX_COLUMNS = 512;
const MAX_CSI_LENGTH = 16;
const TAB_STOP = 8;

/** Browser key name to the byte sequence the Embedded CLI expects. */
const KEY_SEQUENCES = new Map([
  ["Enter", "\r"], ["Tab", "\t"], ["Backspace", "\x7f"], ["Delete", "\x1b[3~"], ["Escape", "\x1b"],
  ["ArrowUp", "\x1b[A"], ["ArrowDown", "\x1b[B"], ["ArrowRight", "\x1b[C"], ["ArrowLeft", "\x1b[D"],
  ["Home", "\x1b[H"], ["End", "\x1b[F"],
]);

/** Cursor-addressed scrollback; rows are arrays of code points so columns match what the CLI counted. */
export class CliScreen {
  /** @type {string[][]} retained rows, oldest first */
  #rows = [[]];
  #row = 0;
  #column = 0;
  /** @type {[number, number]} row and column stored by CSI s */
  #saved = [0, 0];
  /** null outside an escape, "" after ESC, "[" plus parameter bytes while collecting a CSI sequence. */
  #escape = null;

  /** @param {string} text decoded console output */
  write(text) {
    for (const character of text) {
      if (this.#escape !== null) this.#continueEscape(character);
      else if (character === "\x1b") this.#escape = "";
      else if (character === "\r") this.#column = 0;
      else if (character === "\n") this.#lineFeed();
      else if (character === "\b") this.#moveTo(this.#column - 1);
      else if (character === "\t") this.#moveTo((Math.floor(this.#column / TAB_STOP) + 1) * TAB_STOP);
      else if (character >= " " && character !== "\x7f") this.#print(character);
    }
  }

  /**
   * @return {[string, string, string]} text before the cursor, the cursor cell, and text after it.
   * Callers must render these as text nodes, never HTML.
   */
  display() {
    const line = this.#rows[this.#row];
    const text = rows => rows.map(row => row.join(""));
    const lead = Array.from({ length: this.#column }, (_, index) => line[index] ?? " ").join("");
    return [
      [...text(this.#rows.slice(0, this.#row)), lead].join("\n"),
      line[this.#column] ?? " ",
      [line.slice(this.#column + 1).join(""), ...text(this.#rows.slice(this.#row + 1))].join("\n"),
    ];
  }

  #continueEscape(character) {
    if (this.#escape === "") {
      this.#escape = character === "[" ? "[" : null;
    } else if (character >= "@" && character <= "~") {
      this.#control(character, Math.min(MAX_COLUMNS, Number(this.#escape.slice(1)) || 1));
      this.#escape = null;
    } else {
      const valid = /[0-9;]/.test(character) && this.#escape.length < MAX_CSI_LENGTH;
      this.#escape = valid ? this.#escape + character : null;
    }
  }

  #control(final, count) {
    const line = this.#rows[this.#row];
    if (final === "C") this.#moveTo(this.#column + count);
    else if (final === "D") this.#moveTo(this.#column - count);
    else if (final === "s") this.#saved = [this.#row, this.#column];
    else if (final === "u") [this.#row, this.#column] = this.#saved;
    else if (final === "@") {
      line.splice(this.#column, 0, ...Array(count).fill(" "));
      line.length = Math.min(line.length, MAX_COLUMNS);
    } else if (final === "P") line.splice(this.#column, count);
  }

  #print(character) {
    const line = this.#rows[this.#row];
    while (line.length < this.#column) line.push(" ");
    line[this.#column] = character;
    this.#moveTo(this.#column + 1);
  }

  #lineFeed() {
    this.#column = 0;
    if (++this.#row === this.#rows.length) this.#rows.push([]);
    if (this.#rows.length > MAX_ROWS) {
      this.#rows.shift();
      this.#row--;
      this.#saved[0] = Math.max(0, this.#saved[0] - 1);
    }
  }

  #moveTo(column) {
    this.#column = Math.max(0, Math.min(MAX_COLUMNS - 1, column));
  }
}

/**
 * @param {KeyboardEvent} event
 * @return {string | null} bytes to send for the key, or null when the browser should keep it
 */
export function cliInputForKey({ key, ctrlKey, altKey, metaKey }) {
  if (ctrlKey && !altKey && key.length === 1) {
    const code = key.toUpperCase().charCodeAt(0);
    return code >= 0x40 && code <= 0x5f ? String.fromCharCode(code - 0x40) : null;
  }
  if (ctrlKey || altKey || metaKey) return null;
  return KEY_SEQUENCES.get(key) ?? (key.length === 1 ? key : null);
}
