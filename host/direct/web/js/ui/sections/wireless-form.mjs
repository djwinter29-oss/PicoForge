import { OP } from "../../protocol/constants.mjs";
import { bytesToHex } from "../../protocol/codec.mjs";
import { createTimerScope } from "../../timers.mjs";
import { queryElement } from "../dom.mjs";

/**
 * @brief Pure form-operation state machine. One device operation runs at a time.
 * Phases: idle | scanning | testing | canceling | cancelFailed. Unknown or out-of-phase events leave it unchanged.
 * @param form `{phase, cancelError, cancelNeedsReconnect}`.
 * @param event `{type: "scan" | "test" | "done" | "cancel" | "cancelFailed" | "cleared" | "recovered" | "reset",
 *   error?: string, needsReconnect?: boolean}`.
 */
export function formTransition(form, event) {
  const idle = { phase: "idle", cancelError: null, cancelNeedsReconnect: false };
  switch (event.type) {
    case "scan": case "test": return form.phase === "idle" ? { ...form, phase: event.type === "scan"
      ? "scanning" : "testing" } : form;
    case "done": return form.phase === "scanning" ? { ...form, phase: "idle" }
      : form.phase === "testing" ? idle : form;
    case "cancel": return ["idle", "cancelFailed"].includes(form.phase) && !form.cancelNeedsReconnect
      ? { ...form, phase: "canceling" } : form;
    case "cancelFailed": return form.phase === "canceling" ? { phase: "cancelFailed", cancelError: event.error,
      cancelNeedsReconnect: Boolean(event.needsReconnect) } : form;
    case "cleared": return form.phase === "canceling" || form.phase === "idle" ? idle : form;
    case "recovered": return form.phase === "cancelFailed" ? { ...form, cancelNeedsReconnect: false } : form;
    case "reset": return idle;
    default: return form;
  }
}

/**
 * @brief Bind the Wi-Fi/Bluetooth provisioning dialogs. Status comes from the session-polled `state.wireless`
 * passed to `update`; the form performs device I/O only through the injected scan/test/cancelTest intents.
 */
export function bindWirelessForm({
  scan, test, cancelTest, ready, tcpReady = null, supported, changed = () => {}, timerScope,
  now = () => Date.now(), clipboard = null,
}) {
  const timers = timerScope?.createChild() ?? createTimerScope();
  const el = id => queryElement(`#${id}`);
  const wifiDialog = el("direct-wifi-dialog");
  const bleDialog = el("direct-ble-dialog");
  let loaded = false;
  let lifecycleController = new AbortController();
  let status = null;
  let selectedSsidHex;
  let wifiStage = null;
  let wifiTestAttempted = false;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  let form = formTransition({}, { type: "reset" });
  const dispatch = event => { form = formTransition(form, event); };
  let testTimer = null;
  let stagedTimer = null;

  const setStatus = (id, message) => { el(id).textContent = message; };
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const radioOn = id => el(id).value === "1";
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => radioOn("direct-settings-wifi") && wifiStage?.use_tested_wifi
    && remainingTestSeconds() === 0;
  const working = () => form.phase === "scanning" || form.phase === "testing";
  const clearing = () => form.phase === "canceling" || form.phase === "cancelFailed";
  const refresh = () => {
    const available = supported();
    const canSetTcpCli = tcpReady ? tcpReady() : ready() && radioOn("direct-settings-wifi");
    const tcpEnabled = available && canSetTcpCli && !working();
    const wifiBlocked = form.phase !== "idle";
    const tcp = el("direct-settings-tcp-cli");
    tcp.disabled = !tcpEnabled || !status;
    tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    for (const id of ["direct-settings-wifi", "direct-settings-ble"]) {
      const configure = option(id, "configure");
      if (configure) configure.disabled = !ready() || wifiBlocked;
    }
    // Wi-Fi needs a network and Bluetooth needs a control key before either can be enabled.
    const wifiOn = option("direct-settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    const bleOn = option("direct-settings-ble", "1");
    if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
    for (const id of ["direct-wifi-scan", "direct-wifi-other", "direct-wifi-connect", "direct-ble-generate",
      "direct-ble-copy"]) {
      el(id).disabled = !ready() || wifiBlocked;
    }
    el("direct-wifi-connect").disabled ||= !el("direct-wifi-ssid").value;
    el("direct-wifi-connect").textContent = form.phase === "testing" ? "Testing…" : "Test connection";
    el("direct-wifi-cancel").disabled = working() || form.phase === "canceling" || form.cancelNeedsReconnect;
    el("direct-wifi-cancel").textContent = form.phase === "canceling" ? "Clearing…"
      : form.phase === "cancelFailed" ? "Retry clearing test" : "Cancel";
    for (const id of ["direct-wifi-ssid", "direct-wifi-password"]) el(id).disabled = wifiBlocked;
    for (const button of el("direct-wifi-networks").children) button.disabled = wifiBlocked;
    el("direct-ble-copy").disabled ||= !el("direct-ble-secret").value;
    el("direct-wireless-summary").textContent = form.phase === "cancelFailed"
      ? `Could not clear the temporary Wi-Fi test: ${form.cancelError}. ${form.cancelNeedsReconnect
        ? "Reconnect the board before retrying."
        : "Use Retry clearing test before continuing."}`
      : available && radioOn("direct-settings-wifi") && wifiStage?.use_tested_wifi
        ? testExpired() ? "Test credentials expired; test the connection again before saving."
          : `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingTestSeconds()}s.` : "";
    el("direct-wireless-summary").hidden = !el("direct-wireless-summary").textContent;
  };
  /** Disabling a saved-enabled radio erases its credentials at the next save. */
  const erasing = () => Boolean(supported() && status && (
    !radioOn("direct-settings-wifi") && status.wifi_enabled && status.ssid
    || !radioOn("direct-settings-ble") && status.ble_enabled && status.ble_key_configured));
  const dirty = () => Boolean(wifiStage || keyDirty || erasing() || status
    && radioOn("direct-settings-tcp-cli") !== status.tcp_cli_enabled);
  el("direct-settings-tcp-cli").addEventListener("change", () => { refresh(); changed(); });
  const enableRadio = id => {
    if (radioOn(id)) return;
    el(id).value = "1";
    el(id).dispatchEvent?.(new Event("change"));
  };
  const clearBleKey = () => {
    el("direct-ble-secret").value = "";
    el("direct-ble-secret").type = "password";
    keyDirty = false;
  };
  const clearWifiInputs = () => {
    el("direct-wifi-password").value = "";
  };
  /** Forget the staged or edited Wi-Fi pair and its expiry countdown. */
  const clearWifiStage = () => {
    timers.clearInterval(stagedTimer);
    stagedTimer = null;
    wifiStage = null;
    wifiTestAttempted = false;
    wifiEdited = false;
    selectedSsidHex = undefined;
    el("direct-wifi-ssid").value = "";
    clearWifiInputs();
  };
  const renderNetworks = networks => {
    const list = el("direct-wifi-networks");
    list.replaceChildren();
    networks.forEach(network => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "wifi-network";
      button.setAttribute("role", "option");
      button.setAttribute("aria-selected", String(el("direct-wifi-ssid").value === network.ssid));
      const name = document.createElement("span");
      name.textContent = network.ssid;
      const details = document.createElement("small");
      details.textContent = `${network.security ?? (network.secured ? "Secured" : "Open")} · ${network.rssi} dBm`;
      button.append(name, details);
      button.addEventListener("click", () => {
        el("direct-wifi-ssid").value = network.ssid;
        selectedSsidHex = network.ssid_hex;
        wifiEdited = true;
        el("direct-wifi-manual-label").hidden = true;
        el("direct-direct-wifi-password-label").hidden = !network.secured;
        clearWifiInputs();
        refresh();
      });
      list.append(button);
    });
  };
  const discardWifi = async () => {
    if (form.phase === "canceling" || form.cancelNeedsReconnect) return false;
    if (wifiTestAttempted) {
      const signal = lifecycleController.signal;
      dispatch({ type: "cancel" });
      refresh();
      changed();
      try {
        await cancelTest(signal);
      } catch (error) {
        if (signal.aborted) return false;
        dispatch({ type: "cancelFailed", error: error.message, needsReconnect: !ready() });
        setStatus("direct-wifi-status", `Could not clear the temporary Wi-Fi test: ${error.message}`);
        refresh();
        changed();
        return false;
      }
      if (signal.aborted) return false;
    }
    dispatch({ type: "cleared" });
    clearWifiStage();
    refresh();
    changed();
    return true;
  };

  const openWifi = () => {
    el("direct-wifi-ssid").value = wifiStage?.ssid ?? status?.ssid ?? "";
    selectedSsidHex = wifiStage?.ssid_hex ?? status?.ssid_hex;
    el("direct-direct-wifi-password-label").hidden = false;
    el("direct-wifi-manual-label").hidden = false;
    if (form.phase !== "cancelFailed") {
      setStatus("direct-wifi-status", "Choose a network and test the connection. "
        + "Credentials stay in RAM until saved, canceled or expired.");
    }
    wifiDialog.showModal();
  };
  const openBle = () => {
    el("direct-ble-status").textContent = status?.ble_key_configured
      ? "A Control key is saved. It cannot be read back; generate a replacement only if needed."
      : "No Control key is saved. Bluetooth stays off until a key is saved.";
    bleDialog.showModal();
  };
  // "input" fires before "change", so the dashboards' change handlers only ever see Enabled or Disabled.
  for (const [id, open] of /** @type {[string, () => void][]} */ ([["direct-settings-wifi", openWifi],
    ["direct-settings-ble", openBle]])) {
    let previous = el(id).value;
    el(id).addEventListener("focus", () => { previous = el(id).value; });
    el(id).addEventListener("input", () => {
      if (el(id).value === "configure") {
        el(id).value = previous;
        open();
      } else if (previous === "1" && el(id).value === "0" && id === "direct-settings-wifi" && wifiStage) {
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
        if (previous === "0" && id === "direct-settings-ble") clearBleKey();
      }
      refresh();
    });
  }
  el("direct-wifi-scan").addEventListener("click", async () => {
    const signal = lifecycleController.signal;
    dispatch({ type: "scan" });
    setStatus("direct-wifi-status", "Scanning for nearby networks…");
    refresh();
    changed();
    try {
      const networks = await scan(signal);
      if (signal.aborted) return;
      renderNetworks(networks);
      setStatus("direct-wifi-status", networks.length ? `Found ${networks.length} networks.` : "No networks found.");
    } catch (error) {
      if (!signal.aborted) setStatus("direct-wifi-status", error.message);
    } finally {
      if (!signal.aborted) {
        dispatch({ type: "done" });
        refresh();
        changed();
      }
    }
  });
  el("direct-wifi-other").addEventListener("click", () => {
    selectedSsidHex = undefined;
    el("direct-wifi-ssid").value = "";
    el("direct-wifi-manual-label").hidden = false;
    el("direct-direct-wifi-password-label").hidden = false;
    el("direct-wifi-ssid").focus();
  });
  el("direct-wifi-ssid").addEventListener("input", () => {
    selectedSsidHex = undefined;
    wifiEdited = true;
    refresh();
  });
  el("direct-wifi-password").addEventListener("input", () => {
    refresh();
  });
  el("direct-wifi-connect").addEventListener("click", async () => {
    const ssid = el("direct-wifi-ssid").value;
    const password = el("direct-wifi-password").value;
    if (!ssid) return;
    const signal = lifecycleController.signal;
    const started = now();
    dispatch({ type: "test" });
    let step = "Starting";
    const progress = () => {
      const elapsed = Math.floor((now() - started) / 1000);
      setStatus("direct-wifi-status", elapsed < 30
        ? `Testing “${ssid}”: ${step}… ${elapsed}s (up to 30s).`
        : `Testing “${ssid}”: ${step}… ${elapsed}s; waiting for the final result.`);
    };
    progress();
    testTimer = timers.setInterval(progress, 1000);
    refresh();
    changed();
    try {
      wifiTestAttempted = true;
      const rawSsid = selectedSsidHex === undefined ? {} : { ssid_hex: selectedSsidHex };
      const result = await test({ ssid, password, ...rawSsid }, next => {
        if (signal.aborted || !next || next === step) return;
        step = next;
        progress();
      }, signal);
      if (signal.aborted) return;
      if (!Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 ||
          result.remaining_seconds > 600) throw new Error("Firmware returned an invalid staged-credential lifetime");
      wifiStage = { ssid, ...rawSsid, use_tested_wifi: true, tested_at: now(),
        remaining_seconds: result.remaining_seconds };
      timers.clearInterval(stagedTimer);
      stagedTimer = timers.setInterval(refresh, 1000);
      wifiEdited = false;
      enableRadio("direct-settings-wifi");
      clearWifiInputs();
      setStatus("direct-wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save&Reboot.`);
      wifiDialog.close();
      changed();
    } catch (error) {
      if (!signal.aborted) setStatus("direct-wifi-status", error.message);
    } finally {
      if (!signal.aborted) {
        timers.clearInterval(testTimer);
        testTimer = null;
        dispatch({ type: "done" });
        refresh();
        changed();
      }
    }
  });
  el("direct-wifi-cancel").addEventListener("click", async () => {
    if (working()) return;
    if (await discardWifi()) wifiDialog.close();
  });
  wifiDialog.addEventListener("cancel", event => {
    event.preventDefault();
    if (working()) return;
    void discardWifi().then(cleared => { if (cleared) wifiDialog.close(); });
  });
  el("direct-ble-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("direct-ble-secret").value = bytesToHex(bytes);
    bytes.fill(0);
    el("direct-ble-secret").type = "text";
    keyDirty = true;
    keyNeedsRegeneration = false;
    enableRadio("direct-settings-ble");
    setStatus("direct-ble-status", "Copy this key now. The board cannot show it again after saving.");
    refresh();
    changed();
  });
  el("direct-ble-copy").addEventListener("click", async () => {
    try {
      if (!clipboard) throw new Error("Clipboard is unavailable");
      await clipboard.writeText(el("direct-ble-secret").value);
      setStatus("direct-ble-status", "Key copied. Save&Reboot to apply it.");
    } catch (error) { setStatus("direct-ble-status", `Could not copy key: ${error.message}`); }
  });
  el("direct-ble-cancel").addEventListener("click", () => bleDialog.close());

  return {
    dirty,
    operationBusy: () => form.phase !== "idle",
    canSave: () => !keyNeedsRegeneration && !clearing() && !testExpired(),
    request() {
      if (clearing()) {
        throw new Error("Clear the temporary Wi-Fi test before saving.");
      }
      if (testExpired()) throw new Error("Test credentials expired; test the connection again before saving.");
      const wifi = radioOn("direct-settings-wifi");
      const ble = radioOn("direct-settings-ble");
      const config = { wifi_enabled: wifi, ble_enabled: ble,
        ssid: wifi ? wifiStage?.ssid ?? status?.ssid ?? "" : "" };
      const rawSsid = wifi ? wifiStage ? wifiStage.ssid_hex : status?.ssid_hex : undefined;
      if (rawSsid !== undefined) config.ssid_hex = rawSsid;
      if (status) config.tcp_cli_enabled = radioOn("direct-settings-tcp-cli");
      if (wifi && wifiStage?.use_tested_wifi) config.use_tested_wifi = true;
      if (!wifi && status?.ssid) config.password = "";
      if (ble && keyDirty) config.ble_key = el("direct-ble-secret").value;
      else if (!ble && status?.ble_key_configured) config.ble_key = "";
      return config;
    },
    finish(saved, error) {
      const hadKey = keyDirty;
      clearWifiInputs();
      clearBleKey();
      if (saved) {
        status = { ...status, ...saved };
        el("direct-settings-tcp-cli").value = (status.tcp_cli_enabled ?? true) ? "1" : "0";
        clearWifiStage();
        loaded = true;
        keyNeedsRegeneration = false;
        setStatus("direct-wifi-status", "Wireless settings saved.");
        setStatus("direct-ble-status", "");
      } else {
        keyNeedsRegeneration = hadKey;
        setStatus("direct-wifi-status", error);
        setStatus("direct-ble-status", error);
      }
      refresh();
      changed();
    },
    reset() {
      lifecycleController.abort(new DOMException("Wireless form reset", "AbortError"));
      lifecycleController = new AbortController();
      timers.clearInterval(testTimer);
      testTimer = null;
      dispatch({ type: "reset" });
      status = null;
      el("direct-settings-tcp-cli").value = "1";
      loaded = false;
      clearWifiStage();
      keyNeedsRegeneration = false;
      clearBleKey();
      el("direct-wifi-networks").replaceChildren();
      if (wifiDialog.open) wifiDialog.close();
      if (bleDialog.open) bleDialog.close();
    },
    dispose() {
      this.reset();
      timers.dispose();
    },
    /** @brief Adopt the first polled wireless snapshot after (re)connect and apply recovery outcomes. */
    update(state) {
      let recoveredCancellation = false;
      if (form.phase === "cancelFailed" && state?.connected && !state.outcome_uncertain) {
        dispatch({ type: "recovered" });
        if (state.recovered_opcode === OP.CANCEL_TEST) {
          clearWifiStage();
          dispatch({ type: "reset" });
          setStatus("direct-wifi-status", "Temporary Wi-Fi test cleared after reconnect.");
          recoveredCancellation = true;
        }
      }
      if (!loaded && state?.wireless && supported()) {
        status = state.wireless;
        el("direct-settings-tcp-cli").value = (status.tcp_cli_enabled ?? true) ? "1" : "0";
        loaded = true;
        if (!wifiStage && !wifiEdited) el("direct-wifi-ssid").value = status.ssid;
      }
      refresh();
      if (recoveredCancellation) changed();
    },
  };
}
