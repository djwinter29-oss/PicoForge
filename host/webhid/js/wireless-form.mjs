/** Bind the staged Wi-Fi/Bluetooth dialogs to one atomic Board configuration save. */
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

  const setStatus = (id, message) => { el(id).textContent = message; };
  const refresh = () => {
    const available = supported();
    el("wireless-actions").hidden = !available;
    for (const id of ["wifi-open", "ble-open", "wifi-scan", "wifi-other", "wifi-connect",
      "ble-generate", "ble-copy"]) {
      el(id).disabled = !ready() || busy;
    }
    el("wifi-connect").disabled ||= !el("wifi-ssid").value;
    el("wifi-connect").textContent = testTimer !== null ? "Testing…" : "Test connection";
    for (const id of ["wifi-cancel", "wifi-ssid", "wifi-password"]) el(id).disabled = busy;
    for (const button of el("wifi-networks").children) button.disabled = busy;
    el("ble-copy").disabled ||= !el("ble-secret").value;
    const ssid = wifiStage?.ssid ?? status?.ssid ?? "";
    const pending = dirty();
    el("wireless-summary").textContent = !available ? ""
      : wifiStage?.use_tested_wifi ? `Wi-Fi “${ssid}” tested; save to apply.`
        : status?.wifi_state === "connected" ? `Wi-Fi connected to “${ssid}”.`
          : status?.ssid ? `Wi-Fi “${ssid}” is saved; radio ${status.wifi_enabled ? "enabled" : "disabled"}.`
            : "Wi-Fi is not configured.";
    el("wireless-summary").hidden = !available || !pending && !ssid;
  };
  const dirty = () => Boolean(wifiStage || keyDirty);
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
      details.textContent = `${network.secured ? "🔒 " : ""}${network.rssi} dBm`;
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

  el("wifi-open").addEventListener("click", () => {
    el("wifi-ssid").value = wifiStage?.ssid ?? status?.ssid ?? "";
    el("wifi-password-label").hidden = false;
    el("wifi-manual-label").hidden = false;
    setStatus("wifi-status", "Choose a network and test the connection. Credentials are only staged after a successful test.");
    wifiDialog.showModal();
  });
  el("ble-open").addEventListener("click", () => {
    el("ble-status").textContent = status?.ble_key_configured
      ? "A BLE control key is saved. It cannot be read back; generate a replacement only if needed."
      : "No BLE control key is saved.";
    bleDialog.showModal();
  });
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
      wifiStage = { ssid, use_tested_wifi: true };
      wifiEdited = false;
      clearWifiInputs();
      setStatus("wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save and Reboot.`);
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
    setStatus("ble-status", "Copy this key now. The board cannot show it again after saving.");
    refresh();
    changed();
  });
  el("ble-copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(el("ble-secret").value);
      setStatus("ble-status", "Key copied. Save and Reboot to apply it.");
    } catch (error) { setStatus("ble-status", `Could not copy key: ${error.message}`); }
  });
  el("ble-cancel").addEventListener("click", () => bleDialog.close());

  return {
    dirty,
    operationBusy: () => busy,
    canSave: () => !keyNeedsRegeneration,
    request() {
      const ssid = wifiStage?.ssid ?? status?.ssid ?? "";
      const config = { wifi_enabled: Boolean(ssid), ble_enabled: Boolean(keyDirty || status?.ble_key_configured),
        ssid, confirm_reboot: true };
      if (wifiStage?.use_tested_wifi) config.use_tested_wifi = true;
      if (keyDirty) config.ble_key = el("ble-secret").value;
      return config;
    },
    finish(saved, error) {
      const hadKey = keyDirty;
      clearWifiInputs();
      clearBleKey();
      if (saved) {
        status = saved;
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
      testTimer = null;
      busy = false;
      status = null;
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
