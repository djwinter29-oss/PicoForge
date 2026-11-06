// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * Direct Wi-Fi CLI console: an interactive firmware CLI terminal relayed through the Direct local bridge.
 * The dashboard passes only the board address in the URL; firmware always requires TCP auth.
 */
import { CliScreen, cliInputForKey } from "./cli-screen.mjs";
import {
  bytesToHex, CONSOLE_AUTH_ERROR, CONSOLE_AUTH_REQUEST, CONSOLE_AUTH_RESPONSE, parseHexBytes, randomHex,
} from "../protocol/console-messages.mjs";
import { createTimerScope } from "../timers.mjs";

/**
 * @typedef {{session: string, challenge: {nonce: string, uid: string}}} BridgeOpen
 * @typedef {{data: string, closed: boolean}} BridgeRead
 * @typedef {{written: number}} BridgeWrite
 * @typedef {{authenticated: true}} BridgeAuth
 * @typedef {{closed: true}} BridgeClose
 */
/** @overload @param {"open"} operation @param {unknown} value @return {BridgeOpen} */
/** @overload @param {"read"} operation @param {unknown} value @return {BridgeRead} */
/** @overload @param {"write"} operation @param {unknown} value @return {BridgeWrite} */
/** @overload @param {"auth"} operation @param {unknown} value @return {BridgeAuth} */
/** @overload @param {"close"} operation @param {unknown} value @return {BridgeClose} */
/**
 * @brief Validate bridge responses before adopting a session or interpreting terminal output.
 * @param {"open" | "read" | "write" | "auth" | "close"} operation
 * @param {unknown} value
 * @return {BridgeOpen | BridgeRead | BridgeWrite | BridgeAuth | BridgeClose}
 */
export function validateBridgeResponse(operation, value) {
  const invalid = () => new Error(`Invalid Direct terminal bridge ${operation} response`);
  if (!value || typeof value !== "object") throw invalid();
  if (operation === "open" && "session" in value && typeof value.session === "string"
    && /^[A-Za-z0-9_-]{1,128}$/.test(value.session) && "challenge" in value) {
    const challenge = value.challenge;
    if (challenge && typeof challenge === "object" && "nonce" in challenge && typeof challenge.nonce === "string"
      && /^[a-f0-9]{32}$/i.test(challenge.nonce) && "uid" in challenge && typeof challenge.uid === "string"
      && /^[a-f0-9]{16}$/i.test(challenge.uid)) {
      return { session: value.session, challenge: { nonce: challenge.nonce, uid: challenge.uid } };
    }
  } else if (operation === "read" && "data" in value && typeof value.data === "string"
    && /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value.data)
    && "closed" in value && typeof value.closed === "boolean") {
    return { data: value.data, closed: value.closed };
  } else if (operation === "write" && "written" in value && typeof value.written === "number"
    && Number.isInteger(value.written) && value.written >= 1 && value.written <= 4096) {
    return { written: value.written };
  } else if (operation === "auth" && "authenticated" in value && value.authenticated === true) {
    return { authenticated: true };
  } else if (operation === "close" && "closed" in value && value.closed === true) {
    return { closed: true };
  }
  throw invalid();
}

/**
 * @brief Reuse three text-only nodes so cursor edits do not rebuild the entire bounded scrollback DOM.
 * @param {HTMLElement} terminal
 * @param {Document} document
 * @return {(screen: CliScreen) => void}
 */
export function createConsoleRenderer(terminal, document) {
  const beforeText = document.createTextNode("");
  const cursor = document.createElement("span");
  const afterText = document.createTextNode("");
  cursor.className = "terminal-cursor";
  terminal.replaceChildren(beforeText, cursor, afterText);
  return screen => {
    const [before, current, after] = screen.display();
    if (beforeText.data === before && cursor.textContent === current && afterText.data === after) return;
    if (beforeText.data !== before) beforeText.data = before;
    if (cursor.textContent !== current) cursor.textContent = current;
    if (afterText.data !== after) afterText.data = after;
    terminal.scrollTop = terminal.scrollHeight;
  };
}

/**
 * @brief Mount the console on its page. Browser globals are injected by console-app.mjs, the composition root.
 * @param env Page `document`, `window`, `location` and `fetch`.
 */
export function mountConsole({ document, window, location, fetch }) {
  const terminal = document.querySelector("#terminal");
  const status = document.querySelector("#console-status");
  const connect = document.querySelector("#connect");
  const query = new URLSearchParams(location.search);
  const ipv4 = query.get("ipv4") ?? "";
  const encoder = new TextEncoder();
  const timers = createTimerScope();
  const renderScreen = createConsoleRenderer(terminal, document);
  let screen = new CliScreen();
  let decoder = new TextDecoder();
  let session = null;
  let pendingSession = null;
  let sessionController = null;
  let openingController = null;
  let input = "";
  let sending = false;
  /** Pending output frame; input transmission never waits for rendering. */
  let renderFrame = null;
  const flushScreen = () => {
    if (renderFrame !== null) window.cancelAnimationFrame(renderFrame);
    renderFrame = null;
    renderScreen(screen);
  };
  const scheduleScreen = () => {
    if (renderFrame !== null) return;
    renderFrame = window.requestAnimationFrame(() => {
      renderFrame = null;
      renderScreen(screen);
    });
  };

  /** POST one bridge operation with a deadline; aborting the session signal cancels it. */
  /**
   * @param {"open" | "read" | "write" | "auth" | "close"} operation
   * @param {object} payload
   * @param {AbortSignal | undefined} signal
   * @param {number} [timeoutMs]
   * @return {Promise<unknown>} Callers validate the operation-specific response.
   */
  async function bridge(operation, payload, signal, timeoutMs = 10000) {
    const controller = new AbortController();
    const timeout = timers.setTimeout(() => {
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
      /** @type {unknown} */
      let result;
      try { result = await response.json(); } catch {
        throw new Error("The Direct terminal bridge is unavailable. Run tools/host/run-direct.sh from a checkout "
          + "or python3 server.py from the release ZIP.");
      }
      if (!response.ok) {
        const message = result && typeof result === "object" && "error" in result && typeof result.error === "string"
          ? result.error : `HTTP ${response.status}`;
        throw new Error(message);
      }
      return result;
    } finally {
      timers.clearTimeout(timeout);
      signal?.removeEventListener("abort", abort);
    }
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
    flushScreen();
    status.textContent = message;
    terminal.setAttribute("aria-readonly", "true");
    setButton("Connect", true);
    if (token) {
      try { validateBridgeResponse("close", await bridge("close", { session: token }, undefined, 5000)); }
      catch (error) { status.textContent = `${message}. ${error.message}`; }
    }
    connect.disabled = false;
  }

  async function poll(token, signal) {
    while (session === token) {
      try {
        const result = validateBridgeResponse("read", await bridge("read", { session: token }, signal));
        if (session !== token) return;
        const bytes = Uint8Array.from(atob(result.data), character => character.charCodeAt(0));
        const text = decoder.decode(bytes, { stream: !result.closed });
        if (text) {
          screen.write(text);
          scheduleScreen();
        }
        if (result.closed) {
          await close("Connection closed by board; check terminal output before retrying commands");
          return;
        }
        await new Promise(resolve => timers.setTimeout(resolve, 50));
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
        const result = validateBridgeResponse("write",
          await bridge("write", { session: token, data: btoa(String.fromCharCode(...data)) }, signal));
        if (result.written !== data.length) throw new Error("The Direct terminal bridge did not confirm all input bytes");
      }
    } catch (error) {
      if (session === token) await close(`${error.message}; input was not resent`);
    } finally {
      sending = false;
      if (input && session) void send("");
    }
  }

  /** @param {BridgeOpen["challenge"]} challenge @param {AbortSignal} signal @return {Promise<string>} */
  function requestTcpCliProof(challenge, signal) {
    const opener = window.opener;
    if (!opener || typeof opener.postMessage !== "function") {
      throw new Error("Open this console from the Direct dashboard while the matching board is connected.");
    }
    const nonce = parseHexBytes(challenge?.nonce, 16, "console-nonce");
    const uid = parseHexBytes(challenge?.uid, 8, "console-uid");
    const requestId = randomHex();
    return new Promise((resolve, reject) => {
      let timer;
      const finish = (error, proof) => {
        timers.clearTimeout(timer);
        signal.removeEventListener("abort", abort);
        window.removeEventListener("message", receive);
        if (error) reject(error);
        else resolve(proof);
      };
      const abort = () => finish(signal.reason ?? new DOMException("Console request cancelled", "AbortError"));
      const receive = event => {
        if (event.origin !== location.origin || event.source !== opener) return;
        const response = event.data;
        if (response?.request_id !== requestId) return;
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
      window.addEventListener("message", receive);
      timer = timers.setTimeout(() => finish(new Error(
        "No matching authenticated dashboard responded. Connect this board over USB or Bluetooth and try again.")),
      5000);
      try {
        opener.postMessage({ type: CONSOLE_AUTH_REQUEST, request_id: requestId,
          nonce: bytesToHex(nonce), uid: bytesToHex(uid) }, location.origin);
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
      const result = validateBridgeResponse("open", await bridge("open", { ipv4 }, controller.signal, 15000));
      pendingSession = result.session;
      if (controller.signal.aborted) {
        void fetch("/api/direct-terminal/close", { method: "POST", keepalive: true,
          headers: { "Content-Type": "application/json" }, body: JSON.stringify({ session: pendingSession }) });
        pendingSession = null;
        return;
      }
      const proof = await requestTcpCliProof(result.challenge, controller.signal);
      validateBridgeResponse("auth",
        await bridge("auth", { session: result.session, proof }, controller.signal, 5000));
      if (controller.signal.aborted) return;
      session = result.session;
      pendingSession = null;
      openingController = null;
      sessionController = new AbortController();
      screen = new CliScreen();
      decoder = new TextDecoder();
      input = "";
      flushScreen();
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
        try { validateBridgeResponse("close", await bridge("close", { session: pending }, undefined, 5000)); }
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
    const text = cliInputForKey(event);
    if (text !== null) { event.preventDefault(); void send(text); }
  });
  terminal.addEventListener("paste", event => {
    if (!session) return;
    event.preventDefault();
    const text = event.clipboardData.getData("text/plain").replace(/\r\n|\n/g, "\r");
    const warning = "Pasted text includes Enter and may execute commands. Continue?";
    if (text.includes("\r") && !window.confirm(warning)) return;
    void send(text);
  });
  connect.addEventListener("click", () => { void (session ? close() : open()); });
  window.addEventListener("pagehide", () => {
    timers.dispose();
    if (renderFrame !== null) window.cancelAnimationFrame(renderFrame);
    renderFrame = null;
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
  window.addEventListener("pageshow", event => {
    // Recreate disposed timers without reopening a TCP session or resending terminal input.
    if (event.persisted) location.reload();
  });
}
