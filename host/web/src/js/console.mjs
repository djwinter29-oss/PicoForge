// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { ConsoleTerminal, consoleKey } from "./console-terminal.mjs";
import { consoleRequest } from "./console-api.mjs";

const terminal = document.querySelector("#terminal");
const status = document.querySelector("#console-status");
const connect = document.querySelector("#connect");
const board = new URLSearchParams(location.search).get("board") ?? "";
const encoder = new TextEncoder();
let screen = new ConsoleTerminal();
let decoder = new TextDecoder();
let session = null;
let sessionController = null;
let openingController = null;
let input = "";
let sending = false;
let outputFrame = null;

const endpoint = token => `/api/console?session=${encodeURIComponent(token)}`;

function render() {
  const [before, current, after] = screen.display();
  const cursor = document.createElement("span");
  cursor.className = "terminal-cursor";
  cursor.textContent = current;
  terminal.replaceChildren(document.createTextNode(before), cursor, document.createTextNode(after));
  terminal.scrollTop = terminal.scrollHeight;
}

function queueRender() {
  if (outputFrame !== null) return;
  outputFrame = window.requestAnimationFrame(() => {
    outputFrame = null;
    render();
  });
}

function finishRender(flush) {
  if (outputFrame === null) return;
  window.cancelAnimationFrame(outputFrame);
  outputFrame = null;
  if (flush) render();
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
  finishRender(true);
  input = "";
  status.textContent = message;
  terminal.setAttribute("aria-readonly", "true");
  setButton("Connect", true);
  if (token) {
    try { await consoleRequest(endpoint(token), "DELETE", undefined, undefined, 5000); }
    catch (error) { status.textContent = `${message}. ${error.message}`; }
  }
  connect.disabled = false;
}

async function poll(token, signal) {
  while (session === token) {
    try {
      const result = await consoleRequest(endpoint(token), "GET", undefined, signal, 10000);
      if (session !== token) return;
      const bytes = Uint8Array.from(atob(result.data), character => character.charCodeAt(0));
      const text = decoder.decode(bytes, { stream: !result.closed });
      if (text) {
        screen.write(text);
        queueRender();
      }
      if (result.closed) {
        await close("Connection closed by board; check terminal output before retrying commands");
        return;
      }
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
      await consoleRequest(endpoint(token), "PUT", { data: btoa(String.fromCharCode(...data)) }, signal, 10000);
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
  const controller = new AbortController();
  openingController = controller;
  setButton("Connecting…", true);
  status.textContent = "Connecting to the board's TCP CLI...";
  try {
    const result = await consoleRequest(`/api/console?board=${encodeURIComponent(board)}`, "POST",
      {}, controller.signal, 15000);
    if (controller.signal.aborted) {
      void fetch(endpoint(result.session), { method: "DELETE", keepalive: true });
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
connect.addEventListener("click", () => { void (session ? close() : open()); });
window.addEventListener("pagehide", () => {
  finishRender(false);
  openingController?.abort();
  openingController = null;
  const token = session;
  session = null;
  sessionController?.abort();
  sessionController = null;
  if (token) void fetch(endpoint(token), { method: "DELETE", keepalive: true });
});
