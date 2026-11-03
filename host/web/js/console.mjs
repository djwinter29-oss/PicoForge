import { ConsoleTerminal, consoleKey } from "./console-terminal.mjs";

const terminal = document.querySelector("#terminal");
const status = document.querySelector("#console-status");
const connect = document.querySelector("#connect");
const disconnect = document.querySelector("#disconnect");
const authKey = document.querySelector("#console-auth-key");
const board = new URLSearchParams(location.search).get("board") ?? "";
const encoder = new TextEncoder();
let screen = new ConsoleTerminal();
let decoder = new TextDecoder();
let session = null;
let input = "";
let sending = false;

const endpoint = token => `/api/console?session=${encodeURIComponent(token)}`;
async function request(url, method = "GET", payload) {
  const response = await fetch(url, {
    method, cache: "no-store",
    ...(payload === undefined ? {} : {
      headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
    }),
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error ?? result.detail ?? `HTTP ${response.status}`);
  return result;
}

function render() {
  const [before, current, after] = screen.display();
  const cursor = document.createElement("span");
  cursor.className = "terminal-cursor";
  cursor.textContent = current;
  terminal.replaceChildren(document.createTextNode(before), cursor, document.createTextNode(after));
  terminal.scrollTop = terminal.scrollHeight;
}

async function close(message = "Disconnected") {
  const token = session;
  session = null;
  input = "";
  status.textContent = message;
  disconnect.disabled = true;
  terminal.setAttribute("aria-readonly", "true");
  connect.disabled = true;
  if (token) {
    try { await request(endpoint(token), "DELETE"); }
    catch (error) { status.textContent = `${message}. ${error.message}`; }
  }
  connect.disabled = false;
}

async function poll(token) {
  while (session === token) {
    try {
      const result = await request(endpoint(token));
      if (session !== token) return;
      const bytes = Uint8Array.from(atob(result.data), character => character.charCodeAt(0));
      screen.write(decoder.decode(bytes, { stream: !result.closed }));
      render();
      if (result.closed) {
        session = null;
        await close("Connection closed by board; check terminal output before retrying commands");
        return;
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    } catch (error) {
      if (session === token) await close(error.message);
      return;
    }
  }
}

async function send(text) {
  if (!session) return;
  if (encoder.encode(input + text).length > 1024) {
    status.textContent = "Input exceeds 1024 bytes; paste a smaller amount";
    return;
  }
  input += text;
  if (sending) return;
  sending = true;
  const token = session;
  try {
    while (input && session === token) {
      const data = encoder.encode(input);
      input = "";
      await request(endpoint(token), "PUT", { data: btoa(String.fromCharCode(...data)) });
    }
  } catch (error) {
    if (session === token) await close(`${error.message}; input was not resent`);
  } finally {
    sending = false;
    if (input && session) void send("");
  }
}

async function open() {
  connect.disabled = true;
  status.textContent = "Connecting to the board's TCP CLI...";
  try {
    const result = await request(`/api/console?board=${encodeURIComponent(board)}`, "POST",
      authKey.value ? { ble_key: authKey.value } : {});
    session = result.session;
    screen = new ConsoleTerminal();
    decoder = new TextDecoder();
    input = "";
    render();
    status.textContent = "Connected";
    disconnect.disabled = false;
    terminal.setAttribute("aria-readonly", "false");
    terminal.focus();
    void poll(session);
  } catch (error) {
    status.textContent = error.message;
    connect.disabled = false;
  } finally {
    authKey.value = "";
  }
}

terminal.addEventListener("keydown", event => {
  if (!session || event.isComposing || event.key === "Tab" && event.shiftKey) return;
  // Preserve copy/paste and an accessible way out of the terminal with Shift+Tab.
  if ((event.ctrlKey || event.metaKey) && (event.key.toLowerCase() === "v"
    || event.key.toLowerCase() === "c" && window.getSelection()?.toString())) return;
  const text = consoleKey(event);
  if (text !== null) { event.preventDefault(); void send(text); }
});
terminal.addEventListener("paste", event => {
  if (!session) return;
  event.preventDefault();
  const text = event.clipboardData.getData("text/plain").replace(/\r\n|\n/g, "\r");
  if (text.includes("\r") && !window.confirm("Pasted text includes Enter and may execute commands. Continue?")) return;
  void send(text);
});
connect.addEventListener("click", () => { void open(); });
disconnect.addEventListener("click", () => { void close(); });
window.addEventListener("pagehide", () => {
  if (session) void fetch(endpoint(session), { method: "DELETE", keepalive: true });
});
