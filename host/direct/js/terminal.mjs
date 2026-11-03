const endpoint = "/api/direct-terminal/";

function encodeBase64(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function decodeBase64(value) {
  const binary = atob(value);
  return Uint8Array.from(binary, character => character.charCodeAt(0));
}

export function bindDirectTerminal({ dialog, connect, disconnect, close, status, output, input, key, keyLabel,
  getWireless }) {
  let session = null;
  let pollTimer = null;
  let bridgeAvailable = false;
  let bridgeProbe = 0;
  let busy = false;
  let decoder = new TextDecoder();

  const setStatus = message => { status.textContent = message; };
  const updateControls = () => {
    connect.disabled = busy || session !== null || !bridgeAvailable;
    disconnect.disabled = busy || session === null;
    input.disabled = busy || session === null;
    key.disabled = busy || session !== null;
    close.disabled = busy;
  };
  const request = async (operation, payload) => {
    let response;
    try {
      response = await fetch(`${endpoint}${operation}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
    } catch (error) {
      throw new Error(`Could not reach the Direct terminal bridge. Run this dashboard with “python3 server.py”
to use the local terminal. ${error.message}`);
    }
    let result;
    try {
      result = await response.json();
    } catch {
      throw new Error("The Direct terminal bridge is unavailable. Run this dashboard with “python3 server.py”.");
    }
    if (!result || typeof result !== "object") throw new Error("Invalid response from the Direct terminal bridge.");
    if (!response.ok) throw new Error(result.error || `Terminal request failed (${response.status})`);
    return result;
  };
  const checkBridge = async () => {
    const response = await fetch(`${endpoint}health`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
    });
    let result;
    try {
      result = await response.json();
    } catch {
      throw new Error("No Direct local terminal bridge was found.");
    }
    return response.ok && result?.available === true;
  };
  const append = text => {
    output.textContent = (output.textContent + text).slice(-65536);
    output.scrollTop = output.scrollHeight;
  };
  const poll = async token => {
    if (session !== token) return;
    try {
      const result = await request("read", { session: token });
      if (session !== token) return;
      append(decoder.decode(decodeBase64(result.data), { stream: true }));
      if (result.closed) throw new Error("The board closed the terminal connection.");
    } catch (error) {
      if (session === token) {
        session = null;
        decoder = new TextDecoder();
        setStatus(`Terminal disconnected: ${error.message}`);
        updateControls();
      }
      return;
    }
    if (session === token) pollTimer = setTimeout(() => void poll(token), 100);
  };
  const stop = async () => {
    const active = session;
    session = null;
    if (pollTimer !== null) clearTimeout(pollTimer);
    pollTimer = null;
    if (active !== null) {
      try {
        await request("close", { session: active });
        setStatus("Terminal disconnected.");
      } catch (error) {
        setStatus(`Terminal closed locally; the bridge reported: ${error.message}`);
      }
    }
    updateControls();
  };

  connect.addEventListener("click", async () => {
    if (busy || session !== null) return;
    const wireless = getWireless();
    if (!wireless?.wifi_enabled || wireless.wifi_state !== "connected" || !wireless.tcp_cli_started) {
      setStatus("The board Wi-Fi TCP CLI is not available.");
      return;
    }
    const keyValue = key.value;
    if (wireless.tcp_cli_auth_required && !/^(?!0{64})[0-9a-f]{64}$/i.test(keyValue)) {
      keyLabel.hidden = false;
      setStatus("Enter the 64-digit nonzero BLE control key required by this board.");
      key.focus();
      return;
    }
    busy = true;
    updateControls();
    try {
      const result = await request("open", { ipv4: wireless.ipv4,
        auth_required: wireless.tcp_cli_auth_required, ble_key: wireless.tcp_cli_auth_required ? keyValue : null });
      if (typeof result.session !== "string" || !result.session) {
        throw new Error("The Direct terminal bridge did not return a session.");
      }
      session = result.session;
      decoder = new TextDecoder();
      output.textContent = "";
      setStatus(`Connected to ${wireless.ipv4}:2323.`);
      poll(session);
    } catch (error) {
      setStatus(`Could not connect: ${error.message}`);
    } finally {
      key.value = "";
      busy = false;
      updateControls();
    }
  });
  disconnect.addEventListener("click", () => { void stop(); });
  close.addEventListener("click", () => dialog.close());
  input.addEventListener("keydown", async event => {
    if (event.key !== "Enter" || event.shiftKey) return;
    event.preventDefault();
    if (session === null || busy || !input.value) return;
    const active = session;
    const data = new TextEncoder().encode(`${input.value}\r\n`);
    if (data.length > 4096) {
      setStatus("Terminal input exceeds 4096 UTF-8 bytes.");
      return;
    }
    input.value = "";
    busy = true;
    updateControls();
    try {
      await request("write", { session: active, data: encodeBase64(data) });
      if (session === active) input.focus();
    } catch (error) {
      if (session === active) {
        session = null;
        if (pollTimer !== null) clearTimeout(pollTimer);
        pollTimer = null;
        setStatus(`Terminal disconnected: ${error.message}`);
      }
    } finally {
      busy = false;
      updateControls();
    }
  });
  dialog.addEventListener("cancel", event => {
    if (busy) event.preventDefault();
  });
  dialog.addEventListener("close", () => {
    bridgeProbe++;
    bridgeAvailable = false;
    key.value = "";
    void stop();
    updateControls();
  });
  keyLabel.hidden = true;
  setStatus("Connect to the board Wi-Fi CLI to begin.");
  updateControls();

  return {
    async open() {
      const probe = ++bridgeProbe;
      bridgeAvailable = false;
      keyLabel.hidden = true;
      key.value = "";
      setStatus("Checking for the Direct local terminal bridge…");
      if (!dialog.open) dialog.showModal();
      updateControls();
      try {
        bridgeAvailable = await checkBridge();
        if (probe !== bridgeProbe || !dialog.open) return;
        if (!bridgeAvailable) {
          setStatus("The Direct local terminal bridge is unavailable. Download the dashboard ZIP and run "
            + "“python3 server.py”.");
          return;
        }
        const wireless = getWireless();
        keyLabel.hidden = !wireless?.tcp_cli_auth_required;
        setStatus("Connect to the board Wi-Fi CLI to begin.");
      } catch (error) {
        if (probe !== bridgeProbe || !dialog.open) return;
        setStatus(`Could not verify the Direct local terminal bridge: ${error.message}`);
      } finally {
        if (probe === bridgeProbe && dialog.open) updateControls();
      }
    },
  };
}
