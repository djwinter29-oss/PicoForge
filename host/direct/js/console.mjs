/**
 * Direct Wi-Fi CLI console: the Python dashboard's interactive terminal, relayed through the Direct local bridge.
 * The dashboard passes the board address and TCP authentication flag in the URL, read from live wireless status.
 */
import { ConsoleTerminal, consoleKey } from "./console-terminal.mjs?v=1";

const terminal = document.querySelector("#terminal");
const status = document.querySelector("#console-status");
const connect = document.querySelector("#connect");
const authKey = document.querySelector("#console-auth-key");
const query = new URLSearchParams(location.search);
const ipv4 = query.get("ipv4") ?? "";
const authRequired = query.get("auth") === "1";
const encoder = new TextEncoder();
let screen = new ConsoleTerminal();
let decoder = new TextDecoder();
let session = null;
let sessionController = null;
let openingController = null;
let input = "";
let sending = false;

/** POST one bridge operation with a deadline; aborting the session signal cancels it. */
async function bridge(operation, payload, signal, timeoutMs = 10000) {
  const controller = new AbortController();
  const timeout = setTimeout(() => {
    controller.abort(new DOMException("Console request timed out", "TimeoutError"));
  }, timeoutMs);
  const abort = () => controller.abort(signal.reason);
  if (signal?.aborted) abort();
  else signal?.addEventListener("abort", abort, { once: true });
  try {
    let response;
    try {
      response = await fetch(`/api/direct-terminal/${operation}`, { method: "POST", cache: "no-store",
        signal: controller.signal, headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) });
    } catch (error) {
      if (controller.signal.aborted) throw controller.signal.reason ?? error;
      throw new Error("Could not reach the Direct terminal bridge. Run tools/host/run-direct.sh from a checkout "
        + "or python3 server.py from the release ZIP.");
    }
    let result;
    try { result = await response.json(); } catch {
      throw new Error("The Direct terminal bridge is unavailable. Run tools/host/run-direct.sh from a checkout "
        + "or python3 server.py from the release ZIP.");
    }
    if (!response.ok) throw new Error(result?.error ?? `HTTP ${response.status}`);
    return result;
  } finally {
    clearTimeout(timeout);
    signal?.removeEventListener("abort", abort);
  }
}

function render() {
  const [before, current, after] = screen.display();
  const cursor = document.createElement("span");
  cursor.className = "terminal-cursor";
  cursor.textContent = current;
  terminal.replaceChildren(document.createTextNode(before), cursor, document.createTextNode(after));
  terminal.scrollTop = terminal.scrollHeight;
}

/** Show one Connect/Disconnect toggle; the connected state also drives the top-bar light via CSS. */
function setButton(label, disabled, connected = false) {
  connect.textContent = label;
  connect.disabled = disabled;
  connect.setAttribute("data-state", connected ? "connected" : "disconnected");
}

async function close(message = "Disconnected") {
  const token = session;
  const controller = sessionController;
  session = null;
  sessionController = null;
  controller?.abort();
  input = "";
  status.textContent = message;
  terminal.setAttribute("aria-readonly", "true");
  setButton("Connect", true);
  if (token) {
    try { await bridge("close", { session: token }, undefined, 5000); }
    catch (error) { status.textContent = `${message}. ${error.message}`; }
  }
  connect.disabled = false;
}

async function poll(token, signal) {
  while (session === token) {
    try {
      const result = await bridge("read", { session: token }, signal);
      if (session !== token) return;
      const bytes = Uint8Array.from(atob(result.data), character => character.charCodeAt(0));
      const text = decoder.decode(bytes, { stream: !result.closed });
      if (text) {
        screen.write(text);
        render();
      }
      if (result.closed) {
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
  const signal = sessionController?.signal;
  try {
    while (input && session === token) {
      const data = encoder.encode(input);
      input = "";
      await bridge("write", { session: token, data: btoa(String.fromCharCode(...data)) }, signal);
    }
  } catch (error) {
    if (session === token) await close(`${error.message}; input was not resent`);
  } finally {
    sending = false;
    if (input && session) void send("");
  }
}

async function open() {
  if (openingController || session) return;
  if (authRequired && !authKey.value) {
    status.textContent = "This board requires its 64-digit BLE control key";
    authKey.focus();
    return;
  }
  const controller = new AbortController();
  openingController = controller;
  setButton("Connecting…", true);
  status.textContent = "Connecting to the board's TCP CLI...";
  try {
    const result = await bridge("open", { ipv4, auth_required: authRequired,
      ble_key: authRequired ? authKey.value : null }, controller.signal, 15000);
    if (controller.signal.aborted) {
      void fetch("/api/direct-terminal/close", { method: "POST", keepalive: true,
        headers: { "Content-Type": "application/json" }, body: JSON.stringify({ session: result.session }) });
      return;
    }
    session = result.session;
    openingController = null;
    sessionController = new AbortController();
    screen = new ConsoleTerminal();
    decoder = new TextDecoder();
    input = "";
    render();
    status.textContent = "Connected";
    setButton("Disconnect", false, true);
    terminal.setAttribute("aria-readonly", "false");
    terminal.focus();
    void poll(session, sessionController.signal);
  } catch (error) {
    if (!controller.signal.aborted) {
      status.textContent = error.message;
      setButton("Connect", false);
    }
  } finally {
    if (openingController === controller) openingController = null;
    authKey.value = "";
  }
}

authKey.closest("label").hidden = !authRequired;
document.querySelector("#console-target").textContent = ipv4 ? `${ipv4}:2323` : "no board address";
if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(ipv4) || ipv4 === "0.0.0.0") {
  connect.disabled = true;
  status.textContent = "Open this console from the Wi-Fi value on the Direct dashboard";
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
connect.addEventListener("click", () => { void (session ? close() : open()); });
window.addEventListener("pagehide", () => {
  openingController?.abort();
  openingController = null;
  const token = session;
  session = null;
  sessionController?.abort();
  sessionController = null;
  if (token) {
    void fetch("/api/direct-terminal/close", { method: "POST", keepalive: true,
      headers: { "Content-Type": "application/json" }, body: JSON.stringify({ session: token }) });
  }
});
