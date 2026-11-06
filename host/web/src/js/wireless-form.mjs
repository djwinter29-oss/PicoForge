// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
import { testedWireless } from "./dashboard-api.mjs";

/**
 * @typedef {"wifi" | "ble"} Radio
 * @typedef {"preserve" | "replace" | "erase"} CredentialAction
 * @typedef {{ssid: string, ssid_hex?: string}} WifiPair
 * @typedef {WifiPair & {use_tested_wifi: true, tested_at: number, remaining_seconds: number,
 * test_generation: number, test_id: string}} TestedPair
 * @typedef {WifiPair & {wifi_enabled?: boolean, ble_enabled?: boolean, ble_key_configured?: boolean,
 * tcp_cli_enabled?: boolean, stale?: boolean, key_staged?: boolean, ssid_staged?: boolean,
 * password_staged?: boolean, pair_tested?: boolean, test_generation?: number}} WirelessConfiguration
 * @typedef {WirelessConfiguration & {wifi_state?: string, ble_state?: string, ipv4?: string,
 * tcp_cli_started?: boolean}} WirelessStatus
 * @typedef {{wifi_enabled: boolean, ble_enabled: boolean, confirm_reboot: true, tcp_cli_enabled?: boolean,
 * ssid?: string, ssid_hex?: string, password?: string, ble_key?: string,
 * use_tested_wifi?: true, test_id?: string, test_generation?: number}} WirelessSave
 */

/**
 * Show live radio state: Wi-Fi as "SSID (IP)" when connected or "SSID (state)" otherwise; Bluetooth as
 * "PicoForge-<USB serial> (state)". Either shows Disabled when switched off or unconfigured.
 * @param {{wireless?: WirelessStatus | null, connected?: boolean, wireless_error?: string | null,
 * wireless_cleanup_required?: boolean, console_url?: string, hardware?: {capabilities?: number,
 * serial_number?: string} | null}} snapshot
 */
export function renderWirelessStatus(snapshot) {
  const wireless = snapshot.wireless;
  const unavailable = !snapshot.connected ? "--" : snapshot.wireless_error ? "Unavailable" : !wireless ? "--" : null;
  const consoleReady = !unavailable && !snapshot.wireless_cleanup_required && !wireless?.stale
    && wireless?.wifi_enabled && wireless.wifi_state === "connected"
    && wireless.tcp_cli_started && snapshot.console_url;
  /** @param {string} name @param {() => string} text */
  const render = (name, text) => {
    const diagnostics = /** @type {HTMLElement} */ (document.querySelector(`#${name}-diagnostics`));
    diagnostics.hidden = !((snapshot.hardware?.capabilities ?? 0) & 128);
    const status = /** @type {HTMLElement} */ (document.querySelector(`#${name}-state`));
    const label = unavailable ?? text();
    if (name === "wifi" && consoleReady) {
      const existing = status.querySelector?.("a");
      const link = existing ?? document.createElement("a");
      link.textContent = label;
      link.href = consoleReady;
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      link.title = "Open the Wi-Fi CLI console in a new tab";
      if (!existing) status.replaceChildren(link);
    } else status.textContent = label;
    status.title = snapshot.wireless_error ?? (name === "wifi" && snapshot.hardware?.serial_number
      ? `picoforge-${snapshot.hardware.serial_number}.local` : "");
  };
  render("wifi", () => !wireless?.wifi_enabled || !wireless.ssid ? "Disabled"
    : `${wireless.ssid} (${wireless.wifi_state === "connected" ? wireless.ipv4
      : wireless.wifi_state === "connecting" ? "connecting" : "disconnected"})`);
  render("ble", () => {
    if (!wireless?.ble_enabled) return "Disabled";
    if (!wireless.ble_key_configured && wireless.ble_state === "off") return "Off (no control key)";
    const name = snapshot.hardware?.serial_number ? `PicoForge-${snapshot.hardware.serial_number}` : "PicoForge";
    const state = wireless.ble_state === "connected" ? "connected"
      : wireless.ble_state === "advertising" ? "advertising" : "starting";
    return `${name} (${state})`;
  });
}

/** Bind the staged Wi-Fi/Bluetooth dialogs to one atomic boot configuration save.
 * @param {{scan: () => Promise<import("./dashboard-api.mjs").WirelessNetwork[]>,
 * test: (settings: WifiPair & {password: string}, progress: (phase: string) => void) =>
 * Promise<ReturnType<typeof testedWireless>>, cancelTest: () => Promise<unknown>,
 * ready: () => boolean, supported: () => boolean, cleanupRequired?: () => boolean, changed?: () => void}} options
 */
export function bindWirelessForm({
  scan, test, cancelTest, ready, supported, cleanupRequired = () => false, changed = () => {},
}) {
  /** @param {string} id */
  const el = id => {
    const node = document.querySelector(`#${id}`);
    if (!node) throw new Error(`Missing wireless control: ${id}`);
    return /** @type {HTMLElement} */ (node);
  };
  /** @param {string} id */
  const input = id => {
    return /** @type {HTMLInputElement} */ (el(id));
  };
  /** @param {string} id */
  const select = id => /** @type {HTMLSelectElement} */ (el(id));
  /** @param {string} id */
  const button = id => /** @type {HTMLButtonElement} */ (el(id));
  const wifiDialog = /** @type {HTMLDialogElement} */ (el("wifi-dialog"));
  const bleDialog = /** @type {HTMLDialogElement} */ (el("ble-dialog"));
  // Reset advances the form lifetime; late test, cleanup and clipboard continuations must not publish into it.
  let generation = 0;
  /** @type {WirelessConfiguration | null} */
  let status = null;
  /** @type {Partial<Record<"wifi" | "ble" | "tcp", boolean>>} */
  let draft = {};
  let ssid = "";
  let password = "";
  let bleKey = "";
  let configurationConflict = false;
  let foreignStage = false;
  let awaitingPair = false;
  /** @type {TestedPair | null} */
  let wifiStage = null;
  /** @type {WifiPair | null} */
  let wifiAttempt = null;
  /** @type {string | null} */
  let selectedSsidHex = null;
  let wifiEdited = false;
  let keyDirty = false;
  let keyNeedsRegeneration = false;
  let keyRevision = 0;
  let busy = false;
  let wifiCanceling = false;
  /** @type {string | null} */
  let wifiCancelError = null;
  let wifiCancelRequiresReconnect = false;
  /** @type {ReturnType<typeof setInterval> | null} */
  let testTimer = null;
  /** @type {ReturnType<typeof setInterval> | null} */
  let stagedTimer = null;

  /** @param {string} id @param {string | null | undefined} message */
  const setStatus = (id, message) => {
    const text = message ?? "";
    if (el(id).textContent !== text) el(id).textContent = text;
  };
  /** @param {string} id @param {string} value */
  const option = (id, value) => Array.from(select(id).options)
    .find(item => item.value === value);
  /** @param {string} id */
  const radioOn = id => id === "settings-wifi" ? draft.wifi ?? status?.wifi_enabled ?? false
    : id === "settings-ble" ? draft.ble ?? status?.ble_enabled ?? false
      : draft.tcp ?? status?.tcp_cli_enabled ?? true;
  /** @param {Radio} radio */
  const radioEdited = radio => draft[radio] !== undefined;
  const radiosDirty = () => radioEdited("wifi") || radioEdited("ble");
  /** @param {Radio} radio @param {boolean} value */
  const editRadio = (radio, value) => {
    const saved = radio === "wifi" ? status?.wifi_enabled : status?.ble_enabled;
    if (value === saved) delete draft[radio];
    else draft[radio] = value;
    select(`settings-${radio}`).value = value ? "1" : "0";
  };
  const synchronize = () => {
    for (const id of ["settings-wifi", "settings-ble", "settings-tcp-cli"])
      select(id).value = radioOn(id) ? "1" : "0";
  };
  const remainingTestSeconds = () => wifiStage
    ? Math.max(0, wifiStage.remaining_seconds - Math.floor((performance.now() - wifiStage.tested_at) / 1000)) : 0;
  const testExpired = () => radioOn("settings-wifi") && wifiStage?.use_tested_wifi && remainingTestSeconds() === 0;
  const saveBlockReason = () => configurationConflict
    ? "Wireless configuration changed on the board. Discard wireless edits and reload its configuration before saving."
    : radiosDirty() && !status ? "Waiting for fresh wireless configuration before saving radio changes."
    : foreignStage ? "Clear the temporary wireless credentials in Wi-Fi settings before saving; another client staged them."
    : awaitingPair ? "Waiting for status matching the tested pair. If it was replaced, test again or clear the test."
    : busy ? "Finish the wireless operation before saving."
    : wifiCanceling || wifiCancelError || cleanupRequired() ? "Clear the temporary Wi-Fi test before saving."
      : wifiAttempt && !wifiStage ? "Test the connection again or clear the temporary Wi-Fi test before saving."
        : radioOn("settings-wifi") && wifiEdited ? "Network details changed; test the connection again before saving."
          : testExpired() ? "Test credentials expired; test the connection again before saving."
            : keyNeedsRegeneration ? "Generate a replacement Control key or disable Bluetooth before saving."
              : radioEdited("wifi") && radioOn("settings-wifi") && !status?.ssid && !wifiStage
                ? "Test a network before enabling Wi-Fi."
                : radioEdited("ble") && radioOn("settings-ble") && !status?.ble_key_configured && !keyDirty
                  ? "Generate a Control key before enabling Bluetooth." : null;
  const refresh = () => {
    const available = supported();
    const tcpReady = available && ready() && !busy && !wifiCancelError && radioOn("settings-wifi");
    const wifiBlocked = busy || wifiCanceling || wifiCancelError !== null || cleanupRequired();
    const tcp = select("settings-tcp-cli");
    if (tcp) {
      tcp.disabled = !tcpReady || !status;
      tcp.closest?.("label")?.toggleAttribute("hidden", !available);
    }
    button("wifi-configure").disabled = !available || !ready() || busy || wifiCanceling;
    button("ble-configure").disabled = !available || !ready() || busy || wifiCanceling || wifiCancelError !== null;
    el("wifi-settings").hidden = el("ble-settings").hidden = !available;
    // Wi-Fi needs a network and Bluetooth needs a control key before either can be enabled.
    const wifiOn = option("settings-wifi", "1");
    if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
    const bleOn = option("settings-ble", "1");
    if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect", "ble-generate", "ble-copy"]) {
      button(id).disabled = !ready() || busy || wifiCanceling || wifiCancelError !== null;
    }
    for (const id of ["wifi-scan", "wifi-other", "wifi-connect"]) button(id).disabled ||= wifiBlocked;
    button("wifi-connect").disabled ||= !ssid;
    button("wifi-connect").textContent = testTimer !== null ? "Testing…" : "Test connection";
    button("wifi-cancel").disabled = busy || wifiCanceling || wifiCancelRequiresReconnect;
    button("wifi-cancel").textContent = wifiCanceling ? "Clearing…"
      : wifiCancelError || cleanupRequired() ? "Retry clearing test" : "Cancel";
    for (const id of ["wifi-ssid", "wifi-password"]) input(id).disabled = wifiBlocked;
    for (const network of Array.from(
      /** @type {HTMLCollectionOf<HTMLButtonElement>} */ (el("wifi-networks").children))) {
      network.disabled = wifiBlocked;
      network.setAttribute("aria-pressed", String(selectedSsidHex !== null
        && network.dataset.ssidHex === selectedSsidHex));
    }
    button("ble-copy").disabled ||= !bleKey;
    setStatus("wireless-summary", wifiCancelError
      ? `Could not clear temporary Wi-Fi credentials: ${wifiCancelError}. ${wifiCancelRequiresReconnect
        ? "Use Re-sync before retrying; if unavailable, reconnect the board and reload the dashboard."
        : "Open Wi-Fi settings to retry."}`
      : available ? saveBlockReason() ?? (radioOn("settings-wifi") && wifiStage?.use_tested_wifi
        ? `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingTestSeconds()}s.` : "") : "");
    el("wireless-summary").hidden = !el("wireless-summary").textContent;
  };
  /** @returns {{wifi: CredentialAction, ble: CredentialAction}} */
  const credentialActions = () => ({
    wifi: radioEdited("wifi") && !radioOn("settings-wifi") && status?.ssid ? "erase"
      : radioOn("settings-wifi") && wifiStage ? "replace" : "preserve",
    ble: radioEdited("ble") && !radioOn("settings-ble") && status?.ble_key_configured ? "erase"
      : radioOn("settings-ble") && keyDirty ? "replace" : "preserve",
  });
  const dirty = () => Boolean(wifiAttempt || keyDirty || Object.values(credentialActions()).includes("erase")
    || radiosDirty() && !status || draft.tcp !== undefined);
  /** @param {WifiPair} wireless */
  const ssidBytes = wireless => wireless.ssid_hex?.toLowerCase()
    ?? [...new TextEncoder().encode(wireless.ssid)].map(byte => byte.toString(16).padStart(2, "0")).join("");
  /** @param {WirelessConfiguration} wireless @param {string} [ssid] */
  const configuration = (wireless, ssid = ssidBytes(wireless)) => JSON.stringify([
    ssid, wireless.wifi_enabled, wireless.ble_enabled,
    wireless.ble_key_configured, wireless.tcp_cli_enabled ?? true,
  ]);
  select("settings-tcp-cli").addEventListener("change", () => {
    const value = select("settings-tcp-cli").value === "1";
    if (value === status?.tcp_cli_enabled) delete draft.tcp;
    else draft.tcp = value;
    refresh();
    changed();
  });
  /** @param {string} id */
  const enableRadio = id => {
    if (radioOn(id)) return;
    editRadio(id === "settings-wifi" ? "wifi" : "ble", true);
  };
  const clearBleKey = () => {
    keyRevision++;
    bleKey = "";
    input("ble-secret").value = "";
    input("ble-secret").type = "password";
    keyDirty = false;
    keyNeedsRegeneration = false;
  };
  const clearWifiInputs = () => {
    password = "";
    input("wifi-password").value = "";
  };
  /** @param {import("./dashboard-api.mjs").WirelessNetwork[]} networks */
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
        input("wifi-ssid").value = ssid = network.ssid;
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
    clearInterval(stagedTimer ?? undefined);
    stagedTimer = null;
    wifiStage = null;
    wifiAttempt = null;
    foreignStage = false;
    awaitingPair = false;
    wifiEdited = false;
    wifiCancelError = null;
    wifiCancelRequiresReconnect = false;
    if (draft.wifi === true && !status?.ssid) editRadio("wifi", false);
    input("wifi-ssid").value = ssid = "";
    selectedSsidHex = null;
    clearWifiInputs();
    refresh();
    changed();
  };
  const discardWifi = async () => {
    if (wifiCanceling || wifiCancelRequiresReconnect) return false;
    const current = generation;
    if (wifiAttempt || foreignStage || awaitingPair || cleanupRequired()) {
      wifiCanceling = true;
      refresh();
      changed();
      try {
        await cancelTest();
        if (current !== generation) return false;
      } catch (error) {
        if (!(error instanceof Error)) throw error;
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
    input("wifi-ssid").value = ssid = wifiStage?.ssid ?? status?.ssid ?? "";
    selectedSsidHex = (wifiStage ? wifiStage.ssid_hex : status?.ssid_hex) ?? null;
    el("wifi-password-label").hidden = false;
    el("wifi-manual-label").hidden = false;
    setStatus("wifi-status", wifiCancelError
      ? `Could not clear the temporary Wi-Fi test: ${wifiCancelError}. ${wifiCancelRequiresReconnect
        ? "Use Re-sync before retrying; if unavailable, reconnect the board and reload the dashboard."
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
  /** @type {Array<[string, () => void]>} */
  const radioDialogs = [["settings-wifi", openWifi], ["settings-ble", openBle]];
  for (const [id, open] of radioDialogs) {
    const radio = id === "settings-wifi" ? "wifi" : "ble";
    button(`${radio}-configure`).addEventListener("click", () => {
      if (!button(`${radio}-configure`).disabled) open();
    });
    const changeRadio = () => {
      const value = select(id).value;
      if (value !== "0" && value !== "1") throw new Error(`Invalid radio value: ${value}`);
      editRadio(radio, value === "1");
      if (value === "0") {
        if (id === "settings-wifi" && wifiAttempt) {
          const current = generation;
          void discardWifi().then(cleared => {
            if (current !== generation) return;
            if (!cleared) { editRadio(radio, true); openWifi(); }
            refresh();
            changed();
          });
        }
        if (id === "settings-ble") clearBleKey();
      }
      refresh();
      changed();
    };
    el(id).addEventListener("change", changeRadio);
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
    } catch (error) {
      if (!(error instanceof Error)) throw error;
      if (current === generation) setStatus("wifi-status", error.message);
    }
    finally { if (current === generation) { busy = false; refresh(); changed(); } }
  });
  el("wifi-other").addEventListener("click", () => {
    if (wifiCancelError) return;
    selectedSsidHex = null;
    input("wifi-ssid").value = ssid = "";
    wifiEdited = true;
    el("wifi-manual-label").hidden = false;
    el("wifi-password-label").hidden = false;
    input("wifi-ssid").focus();
    refresh();
    changed();
  });
  input("wifi-ssid").addEventListener("input", () => {
    ssid = input("wifi-ssid").value;
    selectedSsidHex = null;
    wifiEdited = true;
    refresh();
    changed();
  });
  input("wifi-password").addEventListener("input", () => {
    password = input("wifi-password").value;
    wifiEdited = true;
    refresh();
    changed();
  });
  button("wifi-connect").addEventListener("click", async () => {
    if (!ready() || busy || wifiCanceling || wifiCancelError) return;
    if (!ssid) return;
    const current = generation;
    const started = performance.now();
    let tested = false;
    busy = true;
    clearInterval(stagedTimer ?? undefined);
    stagedTimer = null;
    wifiStage = null;
    awaitingPair = false;
    let phase = "Starting";
    const progress = () => {
      const elapsed = Math.floor((performance.now() - started) / 1000);
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
      wifiAttempt = { ssid, ...(ssidHex !== null ? { ssid_hex: ssidHex } : {}) };
      const result = testedWireless(await test({ ...wifiAttempt, password }, next => {
        if (current !== generation || !next || next === phase) return;
        phase = next;
        progress();
      }));
      if (current !== generation) return;
      wifiStage = { ...wifiAttempt,
        use_tested_wifi: true, tested_at: performance.now(),
        remaining_seconds: result.remaining_seconds, test_generation: result.test_generation, test_id: result.test_id };
      foreignStage = false;
      clearInterval(stagedTimer ?? undefined);
      stagedTimer = setInterval(() => {
        refresh();
        if (!testExpired()) return;
        clearInterval(stagedTimer ?? undefined);
        stagedTimer = null;
        changed();
      }, 1000);
      wifiEdited = false;
      enableRadio("settings-wifi");
      clearWifiInputs();
      setStatus("wifi-status", `Connected to “${ssid}” (${result.ipv4 || "IP assigned"}). Click Save&Reboot.`);
      tested = true;
    } catch (error) {
      if (!(error instanceof Error)) throw error;
      if (current === generation) setStatus("wifi-status", error.message);
    } finally {
      if (current === generation) {
        clearInterval(testTimer ?? undefined);
        testTimer = null;
        busy = false;
        refresh();
        changed();
        if (tested) wifiDialog.close();
      }
    }
  });
  button("wifi-cancel").addEventListener("click", async () => {
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
    input("ble-secret").value = bleKey = [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
    bytes.fill(0);
    input("ble-secret").type = "text";
    keyDirty = true;
    keyRevision++;
    keyNeedsRegeneration = false;
    enableRadio("settings-ble");
    setStatus("ble-status", "Copy this key now. The board cannot show it again after saving.");
    refresh();
    changed();
  });
  button("ble-copy").addEventListener("click", async () => {
    const current = generation;
    const revision = keyRevision;
    try {
      await navigator.clipboard.writeText(bleKey);
      if (current === generation && revision === keyRevision) {
        setStatus("ble-status", "Key copied. Save&Reboot to apply it.");
      }
    } catch (error) {
      if (!(error instanceof Error)) throw error;
      if (current === generation && revision === keyRevision) setStatus("ble-status", `Could not copy key: ${error.message}`);
    }
  });
  el("ble-cancel").addEventListener("click", () => bleDialog.close());

  const reset = () => {
    generation++;
    clearInterval(testTimer ?? undefined);
    clearInterval(stagedTimer ?? undefined);
    testTimer = null;
    stagedTimer = null;
    busy = false;
    wifiCanceling = false;
    wifiCancelError = null;
    wifiCancelRequiresReconnect = false;
    status = null;
    draft = {};
    configurationConflict = false;
    foreignStage = false;
    awaitingPair = false;
    synchronize();
    wifiStage = null;
    wifiAttempt = null;
    selectedSsidHex = null;
    wifiEdited = false;
    clearWifiInputs();
    clearBleKey();
    input("wifi-ssid").value = ssid = "";
    el("wifi-networks").replaceChildren();
    if (wifiDialog.open) wifiDialog.close();
    if (bleDialog.open) bleDialog.close();
  };

  return {
    clearTest: discardWifi,
    conflicted: () => configurationConflict,
    async discardEdits() {
      if (!configurationConflict || !ready() || busy || wifiCanceling) return false;
      const current = generation;
      if (!await discardWifi()) {
        if (current === generation) openWifi();
        return false;
      }
      if (current !== generation) return false;
      reset();
      refresh();
      changed();
      return true;
    },
    /** @param {boolean} completed */
    cancelResolved(completed) {
      wifiCancelRequiresReconnect = false;
      if (completed) testCleared();
      else { refresh(); changed(); }
    },
    dirty,
    radioEdited,
    /** @param {number} saved */
    selectedRadios: saved => (draft.wifi === undefined ? saved & 1 : Number(draft.wifi))
      | (draft.ble === undefined ? saved & 2 : Number(draft.ble) << 1),
    credentialActions,
    operationBusy: () => busy || wifiCanceling || wifiCancelError !== null,
    inFlight: () => busy || wifiCanceling,
    saveBlockReason,
    saveIdentity: () => JSON.stringify([credentialActions(), radioOn("settings-tcp-cli"), keyRevision,
      wifiStage?.test_id ?? null, wifiStage?.test_generation ?? null, wifiStage ? ssidBytes(wifiStage) : null, dirty()]),
    /** @returns {WirelessSave} */
    request() {
      const blocked = saveBlockReason();
      if (blocked) throw new Error(blocked);
      const wifi = radioOn("settings-wifi");
      const ble = radioOn("settings-ble");
      /** @type {WirelessSave} */
      const config = { wifi_enabled: wifi, ble_enabled: ble, confirm_reboot: true };
      if (status && select("settings-tcp-cli")) config.tcp_cli_enabled = radioOn("settings-tcp-cli");
      if (wifi && wifiStage?.use_tested_wifi) {
        config.ssid = wifiStage.ssid;
        if (wifiStage.ssid_hex !== undefined) config.ssid_hex = wifiStage.ssid_hex;
        config.use_tested_wifi = true;
        config.test_generation = wifiStage.test_generation;
        config.test_id = wifiStage.test_id;
      }
      if (credentialActions().wifi === "erase") { config.ssid = ""; config.password = ""; }
      if (ble && keyDirty) config.ble_key = bleKey;
      else if (credentialActions().ble === "erase") config.ble_key = "";
      return config;
    },
    /** @param {WirelessConfiguration | null} saved @param {string} [error] */
    finish(saved, error) {
      const hadKey = keyDirty;
      clearWifiInputs();
      clearBleKey();
      if (saved) {
        status = saved;
        draft = {};
        synchronize();
        clearInterval(stagedTimer ?? undefined);
        stagedTimer = null;
        configurationConflict = false;
        foreignStage = false;
        awaitingPair = false;
        wifiStage = null;
        wifiAttempt = null;
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
    reset,
    update: refresh,
    /** @param {WirelessConfiguration} fresh */
    initialize(fresh) {
      if (!supported() || !ready() || fresh.stale || busy || wifiCanceling || configurationConflict) return;
      const stagedPair = fresh.ssid_staged === true || fresh.password_staged === true || fresh.pair_tested === true;
      foreignStage = fresh.key_staged === true || stagedPair && !wifiAttempt;
      if (foreignStage) { refresh(); return; }
      if (status && (dirty() || radiosDirty() || wifiEdited || keyNeedsRegeneration || wifiCancelError)) {
        // Feature 12 reports the temporary candidate SSID while testing, not the saved network.
        const ownCandidate = wifiStage && fresh.test_generation === wifiStage.test_generation
          && fresh.pair_tested === true && fresh.ssid_staged === true && fresh.password_staged === true
          && ssidBytes(fresh) === ssidBytes(wifiStage);
        awaitingPair = Boolean(wifiStage && !ownCandidate);
        const expectedSsid = stagedPair ? ssidBytes(status) : ssidBytes(fresh);
        if (configuration(status) !== configuration(fresh, expectedSsid)) configurationConflict = true;
      } else {
        status = { ...fresh, tcp_cli_enabled: fresh.tcp_cli_enabled ?? true };
        input("wifi-ssid").value = ssid = status.ssid;
        selectedSsidHex = status.ssid_hex ?? null;
      }
      synchronize();
      refresh();
    },
  };
}
