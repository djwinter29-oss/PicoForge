import { OP } from "../../protocol/constants.mjs";
import { bytesToHex } from "../../protocol/codec.mjs";
import { createTimerScope } from "../../timers.mjs";
import { queryElement } from "../dom.mjs";
import { describe } from "../messages.mjs";
import { BoardError, REASON } from "../../state/errors.mjs";

/**
 * @typedef {{phase: "idle" | "scanning" | "testing" | "canceling" | "cancelFailed",
 *   cancelError: string | null, cancelNeedsReconnect: boolean}} FormState
 * @typedef {{type: "scan" | "test" | "done" | "cancel" | "cancelFailed" | "cleared" | "recovered" | "reset",
 *   error?: string, needsReconnect?: boolean}} FormEvent
 * @typedef {Partial<ReturnType<typeof import("../../protocol/codec.mjs").decodeWireless>>} WirelessStatus
 * @typedef {{ssid: string, ssid_hex?: string, use_tested_wifi: true, tested_at: number,
 *   remaining_seconds: number}} TestedPair
 * @typedef {object} WirelessFormOptions
 * @property {(signal: AbortSignal) => Promise<ReturnType<typeof import("../../protocol/codec.mjs").decodeScanEntry>[]>} scan
 * @property {(settings: import("../../board/operations.mjs").WirelessPair,
 *   onProgress: (progress: import("../../state/reducer.mjs").ReasonValue) => void,
 *   signal: AbortSignal) => Promise<{remaining_seconds: number, ipv4?: string}>} test
 * @property {(signal: AbortSignal) => Promise<void>} cancelTest
 * @property {() => boolean} ready
 * @property {() => boolean} [settingsReady]
 * @property {(() => boolean) | null} [tcpReady]
 * @property {() => boolean} supported
 * @property {() => void} [changed]
 * @property {ReturnType<typeof createTimerScope>} [timerScope]
 * @property {() => number} [now]
 * @property {Pick<Clipboard, "writeText"> | null} [clipboard]
 */

/** @type {Readonly<FormState>} */
const IDLE_FORM = Object.freeze({ phase: "idle", cancelError: null, cancelNeedsReconnect: false });

/**
 * @brief Pure form-operation state machine. One device operation runs at a time.
 * Phases: idle | scanning | testing | canceling | cancelFailed. Unknown or out-of-phase events leave it unchanged.
 * @param {FormState} form
 * @param {FormEvent} event
 * @return {FormState}
 */
export function formTransition(form, event) {
  const idle = { ...IDLE_FORM };
  switch (event.type) {
    case "scan": case "test": return form.phase === "idle" ? { ...form, phase: event.type === "scan"
      ? "scanning" : "testing" } : form;
    case "done": return form.phase === "scanning" ? { ...form, phase: "idle" }
      : form.phase === "testing" ? idle : form;
    case "cancel": return ["idle", "cancelFailed"].includes(form.phase) && !form.cancelNeedsReconnect
      ? { ...form, phase: "canceling" } : form;
    case "cancelFailed": return form.phase === "canceling" ? { phase: "cancelFailed", cancelError: event.error ?? null,
      cancelNeedsReconnect: Boolean(event.needsReconnect) } : form;
    case "cleared": return form.phase === "canceling" || form.phase === "idle" ? idle : form;
    case "recovered": return form.phase === "cancelFailed" ? { ...form, cancelNeedsReconnect: false } : form;
    case "reset": return idle;
    default: return form;
  }
}

/**
 * @brief Bind the Wi-Fi/Bluetooth provisioning dialogs. Status comes from the session-polled `state.wireless`
 * passed to `update`; this controller owns radio/TCP control availability and provisioning drafts.
 * Device I/O runs only through the injected scan/test/cancelTest intents.
 * @param {WirelessFormOptions} options
 */
export function bindWirelessForm({
  scan, test, cancelTest, ready, settingsReady = ready, tcpReady = null, supported, changed = () => {}, timerScope,
  now = () => Date.now(), clipboard = null,
}) {
  const timers = timerScope?.createChild() ?? createTimerScope();
  const el = id => queryElement(`#${id}`);
  const wifiDialog = el("direct-wifi-dialog");
  const bleDialog = el("direct-ble-dialog");
  let loaded = false;
  let lifecycleController = new AbortController();
  /** @type {WirelessStatus | null} */
  let status = null;
  /** @type {string | undefined} */
  let selectedSsidHex;
  /** @type {TestedPair | null} */
  let wifiStage = null;
  let wifiTestAttempted = false;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  let form = { ...IDLE_FORM };
  /** @param {FormEvent} event */
  const dispatch = event => { form = formTransition(form, event); };
  /** @type {ReturnType<typeof timers.setInterval> | null} */
  let testTimer = null;
  /** @type {ReturnType<typeof timers.setInterval> | null} */
  let stagedTimer = null;

  const setStatus = (id, message) => { el(id).textContent = message; };
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const radioOn = id => el(id).value === "1";
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => Boolean(radioOn("direct-settings-wifi") && wifiStage?.use_tested_wifi
    && remainingTestSeconds() === 0);
  const working = () => form.phase === "scanning" || form.phase === "testing";
  const clearing = () => form.phase === "canceling" || form.phase === "cancelFailed";
  const refresh = () => {
    const available = supported();
    const wirelessReady = ready();
    const radioReady = settingsReady();
    const canSetTcpCli = tcpReady ? tcpReady() : wirelessReady && radioOn("direct-settings-wifi");
    const tcpEnabled = available && canSetTcpCli && !working();
    const wifiBlocked = form.phase !== "idle";
    const tcp = el("direct-settings-tcp-cli");
    tcp.disabled = !tcpEnabled || !status;
    tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    for (const id of ["direct-settings-wifi", "direct-settings-ble"]) {
      el(id).disabled = !available || !radioReady || wifiBlocked;
      el(id).closest?.("label")?.toggleAttribute("hidden", !available);
      const configure = option(id, "configure");
      if (configure) configure.disabled = !wirelessReady || wifiBlocked;
    }
    // Wi-Fi needs a network and Bluetooth needs a control key before either can be enabled.
    const wifiOn = option("direct-settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    const bleOn = option("direct-settings-ble", "1");
    if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
    for (const id of ["direct-wifi-scan", "direct-wifi-other", "direct-wifi-connect", "direct-ble-generate",
      "direct-ble-copy"]) {
      el(id).disabled = !wirelessReady || wifiBlocked;
    }
    el("direct-wifi-connect").disabled ||= !el("direct-wifi-ssid").value;
    el("direct-wifi-connect").textContent = form.phase === "testing" ? "Testing…" : "Test connection";
    el("direct-wifi-cancel").disabled = working() || form.phase === "canceling" || form.cancelNeedsReconnect;
    el("direct-wifi-cancel").textContent = form.phase === "canceling" ? "Clearing…"
      : form.phase === "cancelFailed" ? "Retry clearing test" : "Cancel";
    for (const id of ["direct-wifi-ssid", "direct-wifi-password"]) el(id).disabled = wifiBlocked;
    for (const button of el("direct-wifi-networks").children) {
      button.disabled = wifiBlocked;
      button.setAttribute("aria-pressed", String(selectedSsidHex !== undefined
        && button.dataset.ssidHex === selectedSsidHex));
    }
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
  const notify = () => { refresh(); changed(); };
  /** Disabling a saved-enabled radio erases its credentials at the next save. */
  const erasing = () => Boolean(supported() && status && (
    !radioOn("direct-settings-wifi") && status.wifi_enabled && status.ssid
    || !radioOn("direct-settings-ble") && status.ble_enabled && status.ble_key_configured));
  const dirty = () => Boolean(wifiStage || keyDirty || erasing() || status
    && radioOn("direct-settings-tcp-cli") !== status.tcp_cli_enabled);
  el("direct-settings-tcp-cli").addEventListener("change", notify);
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
  /** @param {Awaited<ReturnType<WirelessFormOptions["scan"]>>} networks */
  const renderNetworks = networks => {
    const list = el("direct-wifi-networks");
    list.replaceChildren();
    networks.forEach(network => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "wifi-network";
      button.dataset.ssidHex = network.ssid_hex;
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
      notify();
      try {
        await cancelTest(signal);
      } catch (error) {
        if (signal.aborted) return false;
        dispatch({ type: "cancelFailed", error: describe(error), needsReconnect: !ready() });
        setStatus("direct-wifi-status", `Could not clear the temporary Wi-Fi test: ${describe(error)}`);
        notify();
        return false;
      }
      if (signal.aborted) return false;
    }
    dispatch({ type: "cleared" });
    clearWifiStage();
    notify();
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
        const signal = lifecycleController.signal;
        void discardWifi().then(cleared => {
          if (signal.aborted) return;
          if (cleared) {
            previous = "0";
            changed();
          } else {
            el(id).value = "1";
            previous = "1";
            openWifi();
            notify();
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
    notify();
    try {
      const networks = await scan(signal);
      if (signal.aborted) return;
      renderNetworks(networks);
      setStatus("direct-wifi-status", networks.length ? `Found ${networks.length} networks.` : "No networks found.");
    } catch (error) {
      if (!signal.aborted) setStatus("direct-wifi-status", describe(error));
    } finally {
      if (!signal.aborted) {
        dispatch({ type: "done" });
        notify();
      }
    }
  });
  el("direct-wifi-other").addEventListener("click", () => {
    selectedSsidHex = undefined;
    el("direct-wifi-ssid").value = "";
    el("direct-wifi-manual-label").hidden = false;
    el("direct-direct-wifi-password-label").hidden = false;
    refresh();
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
    let succeeded = false;
    let step = "Starting";
    const progress = () => {
      const elapsed = Math.floor((now() - started) / 1000);
      setStatus("direct-wifi-status", elapsed < 30
        ? `Testing “${ssid}”: ${step}… ${elapsed}s (up to 30s).`
        : `Testing “${ssid}”: ${step}… ${elapsed}s; waiting for the final result.`);
    };
    progress();
    testTimer = timers.setInterval(progress, 1000);
    notify();
    try {
      wifiTestAttempted = true;
      const rawSsid = selectedSsidHex === undefined ? {} : { ssid_hex: selectedSsidHex };
      const result = await test({ ssid, password, ...rawSsid }, next => {
        if (signal.aborted || !next || next === step) return;
        step = describe(next);
        progress();
      }, signal);
      if (signal.aborted) return;
      if (!Number.isInteger(result.remaining_seconds) || result.remaining_seconds < 1 ||
          result.remaining_seconds > 600) throw new BoardError(REASON.LIFETIME_INVALID);
      wifiStage = { ssid, ...rawSsid, use_tested_wifi: true, tested_at: now(),
        remaining_seconds: result.remaining_seconds };
      timers.clearInterval(stagedTimer);
      stagedTimer = timers.setInterval(() => {
        refresh();
        if (remainingTestSeconds() !== 0) return;
        timers.clearInterval(stagedTimer);
        stagedTimer = null;
        if (testExpired()) changed();
      }, 1000);
      wifiEdited = false;
      enableRadio("direct-settings-wifi");
      clearWifiInputs();
      setStatus("direct-wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save&Reboot.`);
      succeeded = true;
    } catch (error) {
      if (!signal.aborted) setStatus("direct-wifi-status", describe(error));
    } finally {
      if (!signal.aborted) {
        timers.clearInterval(testTimer);
        testTimer = null;
        dispatch({ type: "done" });
        notify();
        if (succeeded) wifiDialog.close();
      }
    }
  });
  el("direct-wifi-cancel").addEventListener("click", async () => {
    if (working()) return;
    const signal = lifecycleController.signal;
    if (await discardWifi() && !signal.aborted) wifiDialog.close();
  });
  wifiDialog.addEventListener("cancel", event => {
    event.preventDefault();
    if (working()) return;
    const signal = lifecycleController.signal;
    void discardWifi().then(cleared => { if (cleared && !signal.aborted) wifiDialog.close(); });
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
    notify();
  });
  el("direct-ble-copy").addEventListener("click", async () => {
    const signal = lifecycleController.signal;
    try {
      if (!clipboard) throw new Error("Clipboard is unavailable");
      await clipboard.writeText(el("direct-ble-secret").value);
      if (signal.aborted) return;
      setStatus("direct-ble-status", "Key copied. Save&Reboot to apply it.");
    } catch (error) {
      if (!signal.aborted) setStatus("direct-ble-status", `Could not copy key: ${describe(error)}`);
    }
  });
  el("direct-ble-cancel").addEventListener("click", () => bleDialog.close());

  return {
    dirty,
    operationBusy: () => form.phase !== "idle",
    canSave: () => !keyNeedsRegeneration && !clearing() && !testExpired(),
    /** @return {import("../../board/operations.mjs").WirelessConfiguration | null} */
    request() {
      if (!dirty()) return null;
      if (clearing()) {
        throw new Error("Clear the temporary Wi-Fi test before saving.");
      }
      if (testExpired()) throw new Error("Test credentials expired; test the connection again before saving.");
      const wifi = radioOn("direct-settings-wifi");
      const ble = radioOn("direct-settings-ble");
      /** @type {import("../../board/operations.mjs").WirelessConfiguration} */
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
    /** @param {WirelessStatus | null} saved @param {string} [error] */
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
      notify();
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
    /** @param {import("../../state/reducer.mjs").DashboardState} [state] */
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
