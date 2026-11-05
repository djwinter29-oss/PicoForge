/**
 * Direct Wi-Fi CLI console: the Python dashboard's interactive terminal, relayed through the Direct local bridge.
 * The dashboard passes the board address and TCP authentication flag in the URL, read from live wireless status.
 */
import { ConsoleTerminal, consoleKey } from "./console-terminal.mjs?v=1";
import {
  bytesToHex, CONSOLE_AUTH_CHANNEL, CONSOLE_AUTH_ERROR, CONSOLE_AUTH_REQUEST, CONSOLE_AUTH_RESPONSE,
  parseHexBytes, randomHex,
} from "./console-auth.mjs?v=1";

const terminal = document.querySelector("#terminal");
const status = document.querySelector("#console-status");
const connect = document.querySelector("#connect");
const query = new URLSearchParams(location.search);
const ipv4 = query.get("ipv4") ?? "";
const authRequired = query.get("auth") === "1";
const authToken = query.get("auth_token") ?? "";
const encoder = new TextEncoder();
let screen = new ConsoleTerminal();
let decoder = new TextDecoder();
let session = null;
let pendingSession = null;
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

function requestTcpCliProof(challenge, signal) {
  if (!/^[a-f0-9]{32}$/i.test(authToken)) {
    throw new Error("Open this console from the Direct dashboard while the matching board is connected.");
  }
  if (typeof window.BroadcastChannel !== "function") {
    throw new Error("This browser cannot request authorization from the Direct dashboard.");
  }
  const nonce = parseHexBytes(challenge?.nonce, 16, "TCP CLI nonce");
  const uid = parseHexBytes(challenge?.uid, 8, "TCP CLI board identity");
  const requestId = randomHex();
  const channel = new window.BroadcastChannel(CONSOLE_AUTH_CHANNEL);
  return new Promise((resolve, reject) => {
    let timer;
    const finish = (error, proof) => {
      clearTimeout(timer);
      signal.removeEventListener("abort", abort);
      channel.close();
      if (error) reject(error);
      else resolve(proof);
    };
    const abort = () => finish(signal.reason ?? new DOMException("Console request cancelled", "AbortError"));
    channel.onmessage = event => {
      const response = event.data;
      if (response?.auth_token !== authToken || response.request_id !== requestId) return;
      if (response.type === CONSOLE_AUTH_ERROR) {
        const message = typeof response.error === "string" ? response.error
          : "The dashboard could not authorize this console";
        finish(new Error(message));
      } else if (response.type === CONSOLE_AUTH_RESPONSE) {
        if (typeof response.uid !== "string" || response.uid.toLowerCase() !== challenge.uid.toLowerCase()
          || typeof response.proof !== "string" || !/^[a-f0-9]{32}$/i.test(response.proof)) {
          finish(new Error("The dashboard returned an invalid TCP CLI proof"));
        } else finish(null, response.proof.toLowerCase());
      }
    };
    if (signal.aborted) {
      abort();
      return;
    }
    signal.addEventListener("abort", abort, { once: true });
    timer = setTimeout(() => finish(new Error(
      "No matching authenticated dashboard responded. Connect this board over USB or Bluetooth and try again.")), 5000);
    try {
      channel.postMessage({ type: CONSOLE_AUTH_REQUEST, auth_token: authToken, request_id: requestId,
        nonce: bytesToHex(nonce), uid: bytesToHex(uid) });
    } catch (error) {
      finish(error instanceof Error ? error : new Error("Could not request dashboard authentication"));
    }
  });
}

async function open() {
  if (openingController || session) return;
  const controller = new AbortController();
  openingController = controller;
  setButton("Connecting…", true);
  status.textContent = "Connecting to the board's TCP CLI...";
  try {
    const result = await bridge("open", { ipv4, auth_required: authRequired }, controller.signal, 15000);
    pendingSession = result.session;
    if (controller.signal.aborted) {
      void fetch("/api/direct-terminal/close", { method: "POST", keepalive: true,
        headers: { "Content-Type": "application/json" }, body: JSON.stringify({ session: pendingSession }) });
      pendingSession = null;
      return;
    }
    if (authRequired) {
      const proof = await requestTcpCliProof(result.challenge, controller.signal);
      await bridge("auth", { session: result.session, proof }, controller.signal, 5000);
    }
    if (controller.signal.aborted) return;
    session = result.session;
    pendingSession = null;
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
    const pending = pendingSession;
    pendingSession = null;
    let message = error.message;
    if (pending) {
      try { await bridge("close", { session: pending }, undefined, 5000); }
      catch (closeError) {
        message = `${message}. ${closeError.message}`;
      }
    }
    if (!controller.signal.aborted) {
      status.textContent = message;
      setButton("Connect", false);
    }
  } finally {
    if (openingController === controller) openingController = null;
  }
}

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
  const token = session ?? pendingSession;
  session = null;
  pendingSession = null;
  sessionController?.abort();
  sessionController = null;
  if (token) {
    void fetch("/api/direct-terminal/close", { method: "POST", keepalive: true,
      headers: { "Content-Type": "application/json" }, body: JSON.stringify({ session: token }) });
  }
});
