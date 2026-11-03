/**
 * Show live radio state: Wi-Fi as "SSID (IP)" when connected or "SSID (state)" otherwise; Bluetooth as
 * "PicoForge-<USB serial> (state)". Either shows Disabled when switched off or unconfigured.
 */
export function renderWirelessStatus(snapshot) {
  const wireless = snapshot.wireless;
  const unavailable = !snapshot.connected ? "--" : snapshot.wireless_error ? "Unavailable" : !wireless ? "--" : null;
  const missingAuthKey = !unavailable && wireless.error === 1 && wireless.tcp_cli_auth_required
    && !wireless.ble_key_configured && !wireless.restart_required;
  const consoleReady = !unavailable && wireless?.wifi_enabled && wireless.wifi_state === "connected"
    && wireless.tcp_cli_started && snapshot.console_url;
  const render = (name, text) => {
    document.querySelector(`#${name}-diagnostics`).hidden = !(snapshot.hardware?.capabilities & 128);
    const status = document.querySelector(`#${name}-state`);
    const label = unavailable ?? `${text()}${name === "wifi" && wireless?.restart_required
      ? " · restart required" : ""}${name === "wifi" && missingAuthKey ? " · TCP authentication needs a key" : ""}`;
    if (name === "wifi" && consoleReady) {
      const existing = status.querySelector?.("a");
      const link = existing ?? document.createElement("a");
      link.textContent = label;
      link.href = snapshot.console_url;
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      link.title = "Open the Wi-Fi CLI console in a new tab";
      if (!existing) status.replaceChildren(link);
    } else status.textContent = label;
    status.title = snapshot.wireless_error ?? (name === "wifi" && missingAuthKey
      ? "Wireless initialization failed. Save a BLE control key or disable TCP authentication, then Save&Reboot."
      : name === "wifi" && wireless?.restart_required
      ? "Saved wireless settings differ from the settings applied at boot; restart required."
      : name === "wifi" && snapshot.hardware?.serial_number
        ? `picoforge-${snapshot.hardware.serial_number}.local` : "");
  };
  render("wifi", () => !wireless.wifi_enabled || !wireless.ssid ? "Disabled"
    : `${wireless.ssid} (${wireless.wifi_state === "connected" ? wireless.ipv4
      : wireless.wifi_state === "connecting" ? "connecting" : "disconnected"})`);
  render("ble", () => {
    if (!wireless.ble_enabled) return "Disabled";
    const name = snapshot.hardware?.serial_number ? `PicoForge-${snapshot.hardware.serial_number}` : "PicoForge";
    const state = wireless.ble_state === "connected" ? "connected"
      : wireless.ble_state === "advertising" ? "advertising" : "starting";
    return `${name} (${state})`;
  });
}

/** Bind the staged Wi-Fi/Bluetooth dialogs to one atomic boot configuration save. */
export function bindWirelessForm({ load, scan, test, cancelTest, ready, supported, changed = () => {} }) {
  const el = id => document.querySelector(`#${id}`);
  const wifiDialog = el("wifi-dialog");
  const bleDialog = el("ble-dialog");
  let loading = false;
  let loaded = false;
  let retryAfter = 0;
  let generation = 0;
  let status = null;
  let wifiStage = null;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  let busy = false;
  let testTimer = null;
  let stagedTimer = null;

  const setStatus = (id, message) => { el(id).textContent = message; };
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const radioOn = id => el(id).value === "1";
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((Date.now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => radioOn("settings-wifi") && wifiStage?.use_tested_wifi && remainingTestSeconds() === 0;
  const keyAvailable = () => radioOn("settings-ble") && (keyDirty
    ? /^[a-f0-9]{64}$/i.test(el("ble-secret").value) && !/^0+$/.test(el("ble-secret").value)
    : Boolean(status?.ble_key_configured));
  const refresh = () => {
    const available = supported();
    const tcpReady = available && ready() && !busy && radioOn("settings-wifi");
    const tcp = el("settings-tcp-cli");
    if (tcp) {
      tcp.disabled = !tcpReady || !status?.tcp_cli_configurable;
      tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    }
    const tcpAuth = el("settings-tcp-auth");
    if (tcpAuth) {
      tcpAuth.disabled = !tcpReady || !radioOn("settings-ble") || !radioOn("settings-tcp-cli")
        || !status?.tcp_cli_auth_configurable || !radioOn("settings-tcp-auth") && !keyAvailable();
      tcpAuth.closest?.("label")?.toggleAttribute("hidden", !available);
    }
    for (const id of ["settings-wifi", "settings-ble"]) {
      const configure = option(id, "configure");
      if (configure) configure.disabled = !ready() || busy;
    }
    // Wi-Fi needs a network before it can be enabled; Bluetooth without a key is view-only.
    const wifiOn = option("settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect", "ble-generate", "ble-copy"]) {
      el(id).disabled = !ready() || busy;
    }
    el("wifi-connect").disabled ||= !el("wifi-ssid").value;
    el("wifi-connect").textContent = testTimer !== null ? "Testing…" : "Test connection";
    for (const id of ["wifi-cancel", "wifi-ssid", "wifi-password"]) el(id).disabled = busy;
    for (const button of el("wifi-networks").children) button.disabled = busy;
    el("ble-copy").disabled ||= !el("ble-secret").value;
    el("wireless-summary").textContent = available && radioOn("settings-wifi") && wifiStage?.use_tested_wifi
      ? testExpired() ? "Test credentials expired; test the connection again before saving."
        : `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingTestSeconds()}s.` : "";
    el("wireless-summary").hidden = !el("wireless-summary").textContent;
  };
  /** Disabling a saved-enabled radio erases its credentials at the next save. */
  const erasing = () => Boolean(supported() && status && (
    !radioOn("settings-wifi") && status.wifi_enabled && (status.ssid || status.password_configured)
    || !radioOn("settings-ble") && status.ble_enabled && status.ble_key_configured));
  const dirty = () => Boolean(wifiStage || keyDirty || erasing() || status?.tcp_cli_configurable
    && radioOn("settings-tcp-cli") !== status.tcp_cli_enabled || status?.tcp_cli_auth_configurable
    && radioOn("settings-tcp-auth") !== status.tcp_cli_auth_required);
  for (const id of ["settings-tcp-cli", "settings-tcp-auth"]) {
    el(id)?.addEventListener("change", () => { refresh(); changed(); });
  }
  const enableRadio = id => {
    if (radioOn(id)) return;
    el(id).value = "1";
    el(id).dispatchEvent?.(new Event("change"));
  };
  const clearBleKey = () => {
    el("ble-secret").value = "";
    el("ble-secret").type = "password";
    keyDirty = false;
  };
  const clearWifiInputs = () => {
    el("wifi-password").value = "";
  };
  const renderNetworks = networks => {
    const list = el("wifi-networks");
    list.replaceChildren();
    networks.forEach(network => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "wifi-network";
      button.setAttribute("role", "option");
      button.setAttribute("aria-selected", String(el("wifi-ssid").value === network.ssid));
      const name = document.createElement("span");
      name.textContent = network.ssid;
      const details = document.createElement("small");
      details.textContent = `${network.security ?? (network.secured ? "Secured" : "Open")} · ${network.rssi} dBm`;
      button.append(name, details);
      button.addEventListener("click", () => {
        el("wifi-ssid").value = network.ssid;
        wifiEdited = true;
        el("wifi-manual-label").hidden = true;
        el("wifi-password-label").hidden = !network.secured;
        clearWifiInputs();
        refresh();
      });
      list.append(button);
    });
  };
  const discardWifi = async () => {
    clearInterval(stagedTimer);
    stagedTimer = null;
    if (wifiStage?.use_tested_wifi) {
      try { await cancelTest(); }
      catch (error) { setStatus("wifi-status", `Could not clear the temporary Wi-Fi test: ${error.message}`); }
    }
    wifiStage = null;
    wifiEdited = false;
    el("wifi-ssid").value = "";
    clearWifiInputs();
    refresh();
  };

  const openWifi = () => {
    el("wifi-ssid").value = wifiStage?.ssid ?? status?.ssid ?? "";
    el("wifi-password-label").hidden = false;
    el("wifi-manual-label").hidden = false;
    setStatus("wifi-status", "Choose a network and test the connection. Credentials are only staged after a successful test.");
    wifiDialog.showModal();
  };
  const openBle = () => {
    el("ble-status").textContent = status?.ble_key_configured
      ? "A BLE control key is saved. It cannot be read back; generate a replacement only if needed."
      : "No BLE control key is saved.";
    bleDialog.showModal();
  };
  // "input" fires before "change", so the dashboards' change handlers only ever see Enabled or Disabled.
  for (const [id, open] of [["settings-wifi", openWifi], ["settings-ble", openBle]]) {
    let previous = el(id).value;
    el(id).addEventListener("focus", () => { previous = el(id).value; });
    el(id).addEventListener("input", () => {
      if (el(id).value === "configure") {
        el(id).value = previous;
        open();
      } else {
        previous = el(id).value;
        if (previous === "0" && id === "settings-wifi" && wifiStage) void discardWifi();
        if (previous === "0" && id === "settings-ble") clearBleKey();
      }
      refresh();
    });
  }
  el("wifi-scan").addEventListener("click", async () => {
    busy = true;
    setStatus("wifi-status", "Scanning for nearby networks…");
    refresh();
    changed();
    try {
      const networks = await scan();
      renderNetworks(networks);
      setStatus("wifi-status", networks.length ? `Found ${networks.length} networks.` : "No networks found.");
    } catch (error) { setStatus("wifi-status", error.message); }
    finally { busy = false; refresh(); changed(); }
  });
  el("wifi-other").addEventListener("click", () => {
    el("wifi-manual-label").hidden = false;
    el("wifi-password-label").hidden = false;
    el("wifi-ssid").focus();
  });
  el("wifi-ssid").addEventListener("input", () => { wifiEdited = true; refresh(); });
  el("wifi-password").addEventListener("input", () => {
    refresh();
  });
  el("wifi-connect").addEventListener("click", async () => {
    const ssid = el("wifi-ssid").value;
    const password = el("wifi-password").value;
    if (!ssid) return;
    const current = generation;
    const started = Date.now();
    busy = true;
    let phase = "Starting";
    const progress = () => {
      const elapsed = Math.floor((Date.now() - started) / 1000);
      setStatus("wifi-status", elapsed < 30
        ? `Testing “${ssid}”: ${phase}… ${elapsed}s (up to 30s).`
        : `Testing “${ssid}”: ${phase}… ${elapsed}s; waiting for the final result.`);
    };
    progress();
    testTimer = setInterval(progress, 1000);
    refresh();
    changed();
    try {
      const result = await test({ ssid, password }, next => {
        if (current !== generation || !next || next === phase) return;
        phase = next;
        progress();
      });
      if (current !== generation) return;
      if (!Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 ||
          result.remaining_seconds > 600) throw new Error("Firmware returned an invalid staged-credential lifetime");
      wifiStage = { ssid, use_tested_wifi: true, tested_at: Date.now(),
        remaining_seconds: result.remaining_seconds };
      clearInterval(stagedTimer);
      stagedTimer = setInterval(refresh, 1000);
      wifiEdited = false;
      enableRadio("settings-wifi");
      clearWifiInputs();
      setStatus("wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save&Reboot.`);
      wifiDialog.close();
      changed();
    } catch (error) {
      if (current === generation) setStatus("wifi-status", error.message);
    } finally {
      if (current === generation) {
        clearInterval(testTimer);
        testTimer = null;
        busy = false;
        refresh();
        changed();
      }
    }
  });
  el("wifi-cancel").addEventListener("click", () => {
    if (busy) return;
    void discardWifi().finally(() => wifiDialog.close());
  });
  wifiDialog.addEventListener("cancel", event => {
    event.preventDefault();
    if (busy) return;
    void discardWifi().finally(() => wifiDialog.close());
  });
  el("ble-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("ble-secret").value = [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
    bytes.fill(0);
    el("ble-secret").type = "text";
    keyDirty = true;
    keyNeedsRegeneration = false;
    enableRadio("settings-ble");
    setStatus("ble-status", "Copy this key now. The board cannot show it again after saving.");
    refresh();
    changed();
  });
  el("ble-copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(el("ble-secret").value);
      setStatus("ble-status", "Key copied. Save&Reboot to apply it.");
    } catch (error) { setStatus("ble-status", `Could not copy key: ${error.message}`); }
  });
  el("ble-cancel").addEventListener("click", () => bleDialog.close());

  return {
    dirty,
    operationBusy: () => busy,
    canSave: () => !keyNeedsRegeneration && !testExpired(),
    request() {
      if (testExpired()) throw new Error("Test credentials expired; test the connection again before saving.");
      const wifi = radioOn("settings-wifi");
      const ble = radioOn("settings-ble");
      const config = { wifi_enabled: wifi, ble_enabled: ble,
        ssid: wifi ? wifiStage?.ssid ?? status?.ssid ?? "" : "", confirm_reboot: true };
      if (status?.tcp_cli_configurable) config.tcp_cli_enabled = radioOn("settings-tcp-cli");
      if (status?.tcp_cli_auth_configurable) {
        config.tcp_cli_auth_required = radioOn("settings-tcp-auth") && keyAvailable();
      }
      if (wifi && wifiStage?.use_tested_wifi) config.use_tested_wifi = true;
      if (!wifi && (status?.ssid || status?.password_configured)) config.password = "";
      if (ble && keyDirty) config.ble_key = el("ble-secret").value;
      else if (!ble && status?.ble_key_configured) config.ble_key = "";
      return config;
    },
    finish(saved, error) {
      const hadKey = keyDirty;
      clearWifiInputs();
      clearBleKey();
      if (saved) {
        status = saved;
        if (el("settings-tcp-cli")) el("settings-tcp-cli").value = (saved.tcp_cli_enabled ?? true) ? "1" : "0";
        if (el("settings-tcp-auth")) el("settings-tcp-auth").value = saved.tcp_cli_auth_required ? "1" : "0";
        clearInterval(stagedTimer);
        stagedTimer = null;
        loaded = true;
        wifiStage = null;
        wifiEdited = false;
        keyNeedsRegeneration = false;
        setStatus("wifi-status", "Wireless settings saved.");
        setStatus("ble-status", "");
      } else {
        keyNeedsRegeneration = hadKey;
        setStatus("wifi-status", error);
        setStatus("ble-status", error);
      }
      refresh();
      changed();
    },
    reset() {
      generation++;
      clearInterval(testTimer);
      clearInterval(stagedTimer);
      testTimer = null;
      stagedTimer = null;
      busy = false;
      status = null;
      if (el("settings-tcp-cli")) el("settings-tcp-cli").value = "1";
      if (el("settings-tcp-auth")) el("settings-tcp-auth").value = "0";
      loaded = false;
      retryAfter = 0;
      wifiStage = null;
      wifiEdited = false;
      keyDirty = false;
      keyNeedsRegeneration = false;
      clearWifiInputs();
      clearBleKey();
      el("wifi-ssid").value = "";
      el("wifi-networks").replaceChildren();
      if (wifiDialog.open) wifiDialog.close();
      if (bleDialog.open) bleDialog.close();
    },
    update: refresh,
    async load() {
      if (loading || loaded || Date.now() < retryAfter || !supported() || !ready()) return;
      loading = true;
      const current = generation;
      try {
        const fresh = await load();
        if (generation !== current) return;
        status = fresh;
        if (el("settings-tcp-cli")) el("settings-tcp-cli").value = (fresh.tcp_cli_enabled ?? true) ? "1" : "0";
        if (el("settings-tcp-auth")) el("settings-tcp-auth").value = fresh.tcp_cli_auth_required ? "1" : "0";
        loaded = true;
        if (!wifiStage && !wifiEdited) el("wifi-ssid").value = status.ssid;
      } catch (error) {
        if (generation === current) {
          retryAfter = Date.now() + 5000;
          setStatus("wifi-status", error.message);
        }
      } finally { loading = false; refresh(); }
    },
  };
}
