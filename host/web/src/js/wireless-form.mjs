/**
 * Show live radio state: Wi-Fi as "SSID (IP)" when connected or "SSID (state)" otherwise; Bluetooth as
 * "PicoForge-<USB serial> (state)". Either shows Disabled when switched off or unconfigured.
 */
export function renderWirelessStatus(snapshot) {
  const wireless = snapshot.wireless;
  const unavailable = !snapshot.connected ? "--" : snapshot.wireless_error ? "Unavailable" : !wireless ? "--" : null;
  const consoleReady = !unavailable && !snapshot.wireless_cleanup_required && !wireless?.stale
    && wireless?.wifi_enabled && wireless.wifi_state === "connected"
    && wireless.tcp_cli_started && snapshot.console_url;
  const render = (name, text) => {
    document.querySelector(`#${name}-diagnostics`).hidden = !(snapshot.hardware?.capabilities & 128);
    const status = document.querySelector(`#${name}-state`);
    const label = unavailable ?? text();
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
    status.title = snapshot.wireless_error ?? (name === "wifi" && snapshot.hardware?.serial_number
      ? `picoforge-${snapshot.hardware.serial_number}.local` : "");
  };
  render("wifi", () => !wireless.wifi_enabled || !wireless.ssid ? "Disabled"
    : `${wireless.ssid} (${wireless.wifi_state === "connected" ? wireless.ipv4
      : wireless.wifi_state === "connecting" ? "connecting" : "disconnected"})`);
  render("ble", () => {
    if (!wireless.ble_enabled) return "Disabled";
    if (!wireless.ble_key_configured && wireless.ble_state === "off") return "Off (no control key)";
    const name = snapshot.hardware?.serial_number ? `PicoForge-${snapshot.hardware.serial_number}` : "PicoForge";
    const state = wireless.ble_state === "connected" ? "connected"
      : wireless.ble_state === "advertising" ? "advertising" : "starting";
    return `${name} (${state})`;
  });
}

/** Bind the staged Wi-Fi/Bluetooth dialogs to one atomic boot configuration save. */
export function bindWirelessForm({
  load, scan, test, cancelTest, ready, supported, cleanupRequired = () => false, changed = () => {},
}) {
  const el = id => document.querySelector(`#${id}`);
  const wifiDialog = el("wifi-dialog");
  const bleDialog = el("ble-dialog");
  let loading = false;
  let loaded = false;
  let retryAfter = 0;
  let generation = 0;
  let status = null;
  let wifiStage = null;
  let wifiTestAttempted = false;
  let selectedSsidHex = null;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  let keyRevision = 0;
  let busy = false;
  let wifiCanceling = false;
  let wifiCancelError = null;
  let wifiCancelRequiresReconnect = false;
  let testTimer = null;
  let stagedTimer = null;

  const setStatus = (id, message) => {
    const text = message ?? "";
    if (el(id).textContent !== text) el(id).textContent = text;
  };
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const radioOn = id => el(id).value === "1";
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((Date.now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => radioOn("settings-wifi") && wifiStage?.use_tested_wifi && remainingTestSeconds() === 0;
  const saveBlockReason = () => busy ? "Finish the wireless operation before saving."
    : wifiCanceling || wifiCancelError || cleanupRequired() ? "Clear the temporary Wi-Fi test before saving."
      : wifiTestAttempted && !wifiStage ? "Test the connection again or clear the temporary Wi-Fi test before saving."
        : radioOn("settings-wifi") && wifiEdited ? "Network details changed; test the connection again before saving."
          : testExpired() ? "Test credentials expired; test the connection again before saving."
            : keyNeedsRegeneration ? "Generate a replacement Control key or disable Bluetooth before saving." : null;
  const refresh = () => {
    const available = supported();
    const tcpReady = available && ready() && !busy && !wifiCancelError && radioOn("settings-wifi");
    const wifiBlocked = busy || wifiCanceling || wifiCancelError !== null || cleanupRequired();
    const tcp = el("settings-tcp-cli");
    if (tcp) {
      tcp.disabled = !tcpReady || !status;
      tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    }
    for (const id of ["settings-wifi", "settings-ble"]) {
      const configure = option(id, "configure");
      if (configure) configure.disabled = !ready() || busy || wifiCanceling
        || id === "settings-ble" && wifiCancelError !== null;
    }
    // Wi-Fi needs a network and Bluetooth needs a control key before either can be enabled.
    const wifiOn = option("settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    const bleOn = option("settings-ble", "1");
    if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect", "ble-generate", "ble-copy"]) {
      el(id).disabled = !ready() || busy || wifiCanceling || wifiCancelError !== null;
    }
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect"]) el(id).disabled ||= wifiBlocked;
    el("wifi-connect").disabled ||= !el("wifi-ssid").value;
    el("wifi-connect").textContent = testTimer !== null ? "Testing…" : "Test connection";
    el("wifi-cancel").disabled = busy || wifiCanceling || wifiCancelRequiresReconnect;
    el("wifi-cancel").textContent = wifiCanceling ? "Clearing…"
      : wifiCancelError || cleanupRequired() ? "Retry clearing test" : "Cancel";
    for (const id of ["wifi-ssid", "wifi-password"]) el(id).disabled = wifiBlocked;
    for (const button of el("wifi-networks").children) {
      button.disabled = wifiBlocked;
      button.setAttribute("aria-pressed", String(selectedSsidHex !== null
        && button.dataset.ssidHex === selectedSsidHex));
    }
    el("ble-copy").disabled ||= !el("ble-secret").value;
    setStatus("wireless-summary", wifiCancelError
      ? `Could not clear temporary Wi-Fi credentials: ${wifiCancelError}. ${wifiCancelRequiresReconnect
        ? "Reload the dashboard and reconnect the board before retrying."
        : "Open Wi-Fi settings to retry."}`
      : available ? saveBlockReason() ?? (radioOn("settings-wifi") && wifiStage?.use_tested_wifi
        ? `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingTestSeconds()}s.` : "") : "");
    el("wireless-summary").hidden = !el("wireless-summary").textContent;
  };
  /** Disabling a saved-enabled radio erases its credentials at the next save. */
  const erasing = () => Boolean(supported() && status && (
    !radioOn("settings-wifi") && status.wifi_enabled && status.ssid
    || !radioOn("settings-ble") && status.ble_enabled && status.ble_key_configured));
  const dirty = () => Boolean(wifiTestAttempted || keyDirty || erasing() || status && el("settings-tcp-cli")
    && radioOn("settings-tcp-cli") !== status.tcp_cli_enabled);
  el("settings-tcp-cli")?.addEventListener("change", () => { refresh(); changed(); });
  const enableRadio = id => {
    if (radioOn(id)) return;
    el(id).value = "1";
    el(id).dispatchEvent?.(new Event("change"));
  };
  const clearBleKey = () => {
    keyRevision++;
    el("ble-secret").value = "";
    el("ble-secret").type = "password";
    keyDirty = false;
    keyNeedsRegeneration = false;
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
      button.setAttribute("aria-pressed", "false");
      button.dataset.ssidHex = network.ssid_hex ?? "";
      const name = document.createElement("span");
      name.textContent = network.ssid;
      const details = document.createElement("small");
      details.textContent = `${network.security ?? (network.secured ? "Secured" : "Open")} · ${network.rssi} dBm`;
      button.append(name, details);
      button.addEventListener("click", () => {
        if (wifiCancelError) return;
        el("wifi-ssid").value = network.ssid;
        selectedSsidHex = network.ssid_hex ?? null;
        wifiEdited = true;
        el("wifi-manual-label").hidden = true;
        el("wifi-password-label").hidden = !network.secured;
        clearWifiInputs();
        refresh();
        changed();
      });
      list.append(button);
    });
  };
  const testCleared = () => {
    clearInterval(stagedTimer);
    stagedTimer = null;
    wifiStage = null;
    wifiTestAttempted = false;
    wifiEdited = false;
    wifiCancelError = null;
    wifiCancelRequiresReconnect = false;
    el("wifi-ssid").value = "";
    selectedSsidHex = null;
    clearWifiInputs();
    refresh();
    changed();
  };
  const discardWifi = async () => {
    if (wifiCanceling || wifiCancelRequiresReconnect) return false;
    const current = generation;
    if (wifiTestAttempted || cleanupRequired()) {
      wifiCanceling = true;
      refresh();
      changed();
      try {
        await cancelTest();
        if (current !== generation) return false;
      } catch (error) {
        if (current !== generation) return false;
        wifiCancelError = error.message;
        wifiCancelRequiresReconnect = error.name === "UnknownOutcomeError"
          || ["AbortError", "TimeoutError"].includes(error.name) || error instanceof TypeError;
        setStatus("wifi-status", `Could not clear the temporary Wi-Fi test: ${error.message}`);
        return false;
      } finally {
        if (current === generation) {
          wifiCanceling = false;
          refresh();
          changed();
        }
      }
    }
    testCleared();
    return true;
  };

  const openWifi = () => {
    el("wifi-ssid").value = wifiStage?.ssid ?? status?.ssid ?? "";
    selectedSsidHex = (wifiStage ? wifiStage.ssid_hex : status?.ssid_hex) ?? null;
    el("wifi-password-label").hidden = false;
    el("wifi-manual-label").hidden = false;
    setStatus("wifi-status", wifiCancelError
      ? `Could not clear the temporary Wi-Fi test: ${wifiCancelError}. ${wifiCancelRequiresReconnect
        ? "Reload the dashboard and reconnect the board before retrying."
        : "Retry clearing before continuing."}`
      : "Choose a network and test the connection. Save requires a successful, unexpired test; Cancel clears temporary credentials.");
    wifiDialog.showModal();
  };
  const openBle = () => {
    el("ble-status").textContent = status?.ble_key_configured
      ? "A Control key is saved. It cannot be read back; generate a replacement only if needed."
      : "No Control key is saved. Bluetooth stays off until a key is saved.";
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
        if (previous === "0" && id === "settings-wifi" && wifiTestAttempted) {
          const current = generation;
          void discardWifi().then(cleared => {
            if (current !== generation) return;
            if (!cleared) { el(id).value = previous = "1"; openWifi(); }
            refresh();
            changed();
          });
        }
        if (previous === "0" && id === "settings-ble") clearBleKey();
      }
      refresh();
    });
  }
  el("wifi-scan").addEventListener("click", async () => {
    if (!ready() || busy || wifiCanceling || wifiCancelError) return;
    const current = generation;
    busy = true;
    setStatus("wifi-status", "Scanning for nearby networks…");
    refresh();
    changed();
    try {
      const networks = await scan();
      if (current !== generation) return;
      renderNetworks(networks);
      setStatus("wifi-status", networks.length ? `Found ${networks.length} networks.` : "No networks found.");
    } catch (error) { if (current === generation) setStatus("wifi-status", error.message); }
    finally { if (current === generation) { busy = false; refresh(); changed(); } }
  });
  el("wifi-other").addEventListener("click", () => {
    if (wifiCancelError) return;
    selectedSsidHex = null;
    el("wifi-ssid").value = "";
    wifiEdited = true;
    el("wifi-manual-label").hidden = false;
    el("wifi-password-label").hidden = false;
    el("wifi-ssid").focus();
    refresh();
    changed();
  });
  el("wifi-ssid").addEventListener("input", () => {
    selectedSsidHex = null;
    wifiEdited = true;
    refresh();
    changed();
  });
  el("wifi-password").addEventListener("input", () => {
    wifiEdited = true;
    refresh();
    changed();
  });
  el("wifi-connect").addEventListener("click", async () => {
    if (!ready() || busy || wifiCanceling || wifiCancelError) return;
    const ssid = el("wifi-ssid").value;
    const password = el("wifi-password").value;
    if (!ssid) return;
    const current = generation;
    const started = Date.now();
    let tested = false;
    busy = true;
    clearInterval(stagedTimer);
    stagedTimer = null;
    wifiStage = null;
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
      const ssidHex = selectedSsidHex;
      wifiTestAttempted = true;
      const result = await test({ ssid, password, ...(ssidHex !== null ? { ssid_hex: ssidHex } : {}) }, next => {
        if (current !== generation || !next || next === phase) return;
        phase = next;
        progress();
      });
      if (current !== generation) return;
      if (!Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 ||
          result.remaining_seconds > 600) throw new Error("Firmware returned an invalid staged-credential lifetime");
      wifiStage = { ssid, ...(ssidHex !== null ? { ssid_hex: ssidHex } : {}),
        use_tested_wifi: true, tested_at: Date.now(),
        remaining_seconds: result.remaining_seconds };
      clearInterval(stagedTimer);
      stagedTimer = setInterval(() => {
        refresh();
        if (!testExpired()) return;
        clearInterval(stagedTimer);
        stagedTimer = null;
        changed();
      }, 1000);
      wifiEdited = false;
      enableRadio("settings-wifi");
      clearWifiInputs();
      setStatus("wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save&Reboot.`);
      tested = true;
    } catch (error) {
      if (current === generation) setStatus("wifi-status", error.message);
    } finally {
      if (current === generation) {
        clearInterval(testTimer);
        testTimer = null;
        busy = false;
        refresh();
        changed();
        if (tested) wifiDialog.close();
      }
    }
  });
  el("wifi-cancel").addEventListener("click", async () => {
    if (busy || wifiCanceling) return;
    const current = generation;
    if (await discardWifi() && current === generation) wifiDialog.close();
  });
  wifiDialog.addEventListener("cancel", event => {
    event.preventDefault();
    if (busy) return;
    const current = generation;
    void discardWifi().then(cleared => { if (cleared && current === generation) wifiDialog.close(); });
  });
  el("ble-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("ble-secret").value = [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
    bytes.fill(0);
    el("ble-secret").type = "text";
    keyDirty = true;
    keyRevision++;
    keyNeedsRegeneration = false;
    enableRadio("settings-ble");
    setStatus("ble-status", "Copy this key now. The board cannot show it again after saving.");
    refresh();
    changed();
  });
  el("ble-copy").addEventListener("click", async () => {
    const current = generation;
    const revision = keyRevision;
    try {
      await navigator.clipboard.writeText(el("ble-secret").value);
      if (current === generation && revision === keyRevision) {
        setStatus("ble-status", "Key copied. Save&Reboot to apply it.");
      }
    } catch (error) {
      if (current === generation && revision === keyRevision) setStatus("ble-status", `Could not copy key: ${error.message}`);
    }
  });
  el("ble-cancel").addEventListener("click", () => bleDialog.close());

  return {
    clearTest: discardWifi,
    cancelResolved(completed) {
      wifiCancelRequiresReconnect = false;
      if (completed) testCleared();
      else { refresh(); changed(); }
    },
    dirty,
    operationBusy: () => busy || wifiCanceling || wifiCancelError !== null,
    inFlight: () => busy || wifiCanceling,
    saveBlockReason,
    stageValid: () => saveBlockReason() === null,
    canSave: () => saveBlockReason() === null,
    request() {
      const blocked = saveBlockReason();
      if (blocked) throw new Error(blocked);
      const wifi = radioOn("settings-wifi");
      const ble = radioOn("settings-ble");
      const config = { wifi_enabled: wifi, ble_enabled: ble,
        ssid: wifi ? wifiStage?.ssid ?? status?.ssid ?? "" : "", confirm_reboot: true };
      const ssidHex = wifi ? (wifiStage ? wifiStage.ssid_hex : status?.ssid_hex) : null;
      if (ssidHex !== undefined && ssidHex !== null) config.ssid_hex = ssidHex;
      if (status && el("settings-tcp-cli")) config.tcp_cli_enabled = radioOn("settings-tcp-cli");
      if (wifi && wifiStage?.use_tested_wifi) config.use_tested_wifi = true;
      if (!wifi && status?.ssid) config.password = "";
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
        clearInterval(stagedTimer);
        stagedTimer = null;
        loaded = true;
        wifiStage = null;
        wifiTestAttempted = false;
        selectedSsidHex = null;
        wifiEdited = false;
        wifiCancelError = null;
        wifiCancelRequiresReconnect = false;
        wifiCanceling = false;
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
      wifiCanceling = false;
      wifiCancelError = null;
      wifiCancelRequiresReconnect = false;
      status = null;
      loading = false;
      if (el("settings-tcp-cli")) el("settings-tcp-cli").value = "1";
      loaded = false;
      retryAfter = 0;
      wifiStage = null;
      wifiTestAttempted = false;
      selectedSsidHex = null;
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
        loaded = true;
        if (!wifiStage && !wifiEdited) el("wifi-ssid").value = status.ssid;
      } catch (error) {
        if (generation === current) {
          retryAfter = Date.now() + 5000;
          setStatus("wifi-status", error.message);
        }
      } finally { if (generation === current) { loading = false; refresh(); } }
    },
  };
}
