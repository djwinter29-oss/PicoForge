import { OP, bytesToHex } from "./protocol.mjs?v=48";

export function bindWirelessForm({
  load, scan, test, cancelTest, ready, loadReady = ready, tcpReady = null, supported, changed = () => {},
}) {
  const el = id => document.querySelector(`#${id}`);
  const wifiDialog = el("wifi-dialog");
  const bleDialog = el("ble-dialog");
  let loading = false;
  let loaded = false;
  let retryAfter = 0;
  let generation = 0;
  let status = null;
  let selectedSsidHex;
  let wifiStage = null;
  let wifiTestAttempted = false;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  /** One device operation at a time: idle | scanning | testing | canceling | cancelFailed. */
  let phase = "idle";
  let cancelError = null;
  let cancelNeedsReconnect = false;
  let testTimer = null;
  let stagedTimer = null;

  const setStatus = (id, message) => { el(id).textContent = message; };
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const radioOn = id => el(id).value === "1";
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((Date.now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => radioOn("settings-wifi") && wifiStage?.use_tested_wifi && remainingTestSeconds() === 0;
  const working = () => phase === "scanning" || phase === "testing";
  const clearing = () => phase === "canceling" || phase === "cancelFailed";
  const refresh = () => {
    const available = supported();
    const canSetTcpCli = tcpReady ? tcpReady() : ready() && radioOn("settings-wifi");
    const tcpEnabled = available && canSetTcpCli && !working();
    const wifiBlocked = phase !== "idle";
    const tcp = el("settings-tcp-cli");
    tcp.disabled = !tcpEnabled || !status;
    tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    for (const id of ["settings-wifi", "settings-ble"]) {
      const configure = option(id, "configure");
      if (configure) configure.disabled = !ready() || wifiBlocked;
    }
    // Wi-Fi needs a network and Bluetooth needs a control key before either can be enabled.
    const wifiOn = option("settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    const bleOn = option("settings-ble", "1");
    if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect", "ble-generate", "ble-copy"]) {
      el(id).disabled = !ready() || wifiBlocked;
    }
    el("wifi-connect").disabled ||= !el("wifi-ssid").value;
    el("wifi-connect").textContent = phase === "testing" ? "Testing…" : "Test connection";
    el("wifi-cancel").disabled = working() || phase === "canceling" || cancelNeedsReconnect;
    el("wifi-cancel").textContent = phase === "canceling" ? "Clearing…"
      : phase === "cancelFailed" ? "Retry clearing test" : "Cancel";
    for (const id of ["wifi-ssid", "wifi-password"]) el(id).disabled = wifiBlocked;
    for (const button of el("wifi-networks").children) button.disabled = wifiBlocked;
    el("ble-copy").disabled ||= !el("ble-secret").value;
    el("wireless-summary").textContent = phase === "cancelFailed"
      ? `Could not clear the temporary Wi-Fi test: ${cancelError}. ${cancelNeedsReconnect
        ? "Reconnect the board before retrying."
        : "Use Retry clearing test before continuing."}`
      : available && radioOn("settings-wifi") && wifiStage?.use_tested_wifi
        ? testExpired() ? "Test credentials expired; test the connection again before saving."
          : `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingTestSeconds()}s.` : "";
    el("wireless-summary").hidden = !el("wireless-summary").textContent;
  };
  /** Disabling a saved-enabled radio erases its credentials at the next save. */
  const erasing = () => Boolean(supported() && status && (
    !radioOn("settings-wifi") && status.wifi_enabled && status.ssid
    || !radioOn("settings-ble") && status.ble_enabled && status.ble_key_configured));
  const dirty = () => Boolean(wifiStage || keyDirty || erasing() || status
    && radioOn("settings-tcp-cli") !== status.tcp_cli_enabled);
  el("settings-tcp-cli").addEventListener("change", () => { refresh(); changed(); });
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
  /** Forget the staged or edited Wi-Fi pair and its expiry countdown. */
  const clearWifiStage = () => {
    clearInterval(stagedTimer);
    stagedTimer = null;
    wifiStage = null;
    wifiTestAttempted = false;
    wifiEdited = false;
    selectedSsidHex = undefined;
    el("wifi-ssid").value = "";
    clearWifiInputs();
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
        selectedSsidHex = network.ssid_hex;
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
    if (phase === "canceling" || cancelNeedsReconnect) return false;
    if (wifiTestAttempted) {
      const current = generation;
      phase = "canceling";
      refresh();
      changed();
      try {
        await cancelTest();
      } catch (error) {
        if (current !== generation) return false;
        phase = "cancelFailed";
        cancelError = error.message;
        cancelNeedsReconnect = !ready();
        setStatus("wifi-status", `Could not clear the temporary Wi-Fi test: ${error.message}`);
        refresh();
        changed();
        return false;
      }
      if (current !== generation) return false;
    }
    phase = "idle";
    cancelError = null;
    cancelNeedsReconnect = false;
    clearWifiStage();
    refresh();
    changed();
    return true;
  };

  const openWifi = () => {
    el("wifi-ssid").value = wifiStage?.ssid ?? status?.ssid ?? "";
    selectedSsidHex = wifiStage?.ssid_hex ?? status?.ssid_hex;
    el("wifi-password-label").hidden = false;
    el("wifi-manual-label").hidden = false;
    if (phase !== "cancelFailed") {
      setStatus("wifi-status", "Choose a network and test the connection. "
        + "Credentials stay in RAM until saved, canceled or expired.");
    }
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
      } else if (previous === "1" && el(id).value === "0" && id === "settings-wifi" && wifiStage) {
        void discardWifi().then(cleared => {
          if (cleared) {
            previous = "0";
            changed();
          } else {
            el(id).value = "1";
            previous = "1";
            openWifi();
            refresh();
            changed();
          }
        });
      } else {
        previous = el(id).value;
        if (previous === "0" && id === "settings-ble") clearBleKey();
      }
      refresh();
    });
  }
  el("wifi-scan").addEventListener("click", async () => {
    const current = generation;
    phase = "scanning";
    setStatus("wifi-status", "Scanning for nearby networks…");
    refresh();
    changed();
    try {
      const networks = await scan();
      if (current !== generation) return;
      renderNetworks(networks);
      setStatus("wifi-status", networks.length ? `Found ${networks.length} networks.` : "No networks found.");
    } catch (error) {
      if (current === generation) setStatus("wifi-status", error.message);
    } finally {
      if (current === generation) {
        phase = "idle";
        refresh();
        changed();
      }
    }
  });
  el("wifi-other").addEventListener("click", () => {
    selectedSsidHex = undefined;
    el("wifi-ssid").value = "";
    el("wifi-manual-label").hidden = false;
    el("wifi-password-label").hidden = false;
    el("wifi-ssid").focus();
  });
  el("wifi-ssid").addEventListener("input", () => { selectedSsidHex = undefined; wifiEdited = true; refresh(); });
  el("wifi-password").addEventListener("input", () => {
    refresh();
  });
  el("wifi-connect").addEventListener("click", async () => {
    const ssid = el("wifi-ssid").value;
    const password = el("wifi-password").value;
    if (!ssid) return;
    const current = generation;
    const started = Date.now();
    phase = "testing";
    let step = "Starting";
    const progress = () => {
      const elapsed = Math.floor((Date.now() - started) / 1000);
      setStatus("wifi-status", elapsed < 30
        ? `Testing “${ssid}”: ${step}… ${elapsed}s (up to 30s).`
        : `Testing “${ssid}”: ${step}… ${elapsed}s; waiting for the final result.`);
    };
    progress();
    testTimer = setInterval(progress, 1000);
    refresh();
    changed();
    try {
      wifiTestAttempted = true;
      const rawSsid = selectedSsidHex === undefined ? {} : { ssid_hex: selectedSsidHex };
      const result = await test({ ssid, password, ...rawSsid }, next => {
        if (current !== generation || !next || next === step) return;
        step = next;
        progress();
      });
      if (current !== generation) return;
      if (!Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 ||
          result.remaining_seconds > 600) throw new Error("Firmware returned an invalid staged-credential lifetime");
      wifiStage = { ssid, ...rawSsid, use_tested_wifi: true, tested_at: Date.now(),
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
        phase = "idle";
        cancelError = null;
        cancelNeedsReconnect = false;
        refresh();
        changed();
      }
    }
  });
  el("wifi-cancel").addEventListener("click", async () => {
    if (working()) return;
    if (await discardWifi()) wifiDialog.close();
  });
  wifiDialog.addEventListener("cancel", event => {
    event.preventDefault();
    if (working()) return;
    void discardWifi().then(cleared => { if (cleared) wifiDialog.close(); });
  });
  el("ble-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("ble-secret").value = bytesToHex(bytes);
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
    operationBusy: () => phase !== "idle",
    canSave: () => !keyNeedsRegeneration && !clearing() && !testExpired(),
    request() {
      if (clearing()) {
        throw new Error("Clear the temporary Wi-Fi test before saving.");
      }
      if (testExpired()) throw new Error("Test credentials expired; test the connection again before saving.");
      const wifi = radioOn("settings-wifi");
      const ble = radioOn("settings-ble");
      const config = { wifi_enabled: wifi, ble_enabled: ble,
        ssid: wifi ? wifiStage?.ssid ?? status?.ssid ?? "" : "" };
      const rawSsid = wifi ? wifiStage ? wifiStage.ssid_hex : status?.ssid_hex : undefined;
      if (rawSsid !== undefined) config.ssid_hex = rawSsid;
      if (status) config.tcp_cli_enabled = radioOn("settings-tcp-cli");
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
        status = { ...status, ...saved };
        el("settings-tcp-cli").value = (status.tcp_cli_enabled ?? true) ? "1" : "0";
        clearWifiStage();
        loaded = true;
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
      phase = "idle";
      cancelError = null;
      cancelNeedsReconnect = false;
      status = null;
      el("settings-tcp-cli").value = "1";
      loaded = false;
      retryAfter = 0;
      clearWifiStage();
      keyNeedsRegeneration = false;
      clearBleKey();
      el("wifi-networks").replaceChildren();
      if (wifiDialog.open) wifiDialog.close();
      if (bleDialog.open) bleDialog.close();
    },
    update(state) {
      let recoveredCancellation = false;
      if (phase === "cancelFailed" && state?.connected && !state.outcome_uncertain) {
        cancelNeedsReconnect = false;
        if (state.recovered_opcode === OP.CANCEL_TEST) {
          clearWifiStage();
          phase = "idle";
          cancelError = null;
          setStatus("wifi-status", "Temporary Wi-Fi test cleared after reconnect.");
          recoveredCancellation = true;
        }
      }
      refresh();
      if (recoveredCancellation) changed();
    },
    async load() {
      if (loading || loaded || Date.now() < retryAfter || !supported() || !loadReady()) return;
      loading = true;
      const current = generation;
      try {
        const fresh = await load();
        if (generation !== current) return;
        status = fresh;
        el("settings-tcp-cli").value = (fresh.tcp_cli_enabled ?? true) ? "1" : "0";
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
