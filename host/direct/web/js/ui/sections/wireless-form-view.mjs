/** @file Wireless form presentation: apply a readiness/draft snapshot without performing device I/O. */
import { queryElement } from "../dom.mjs";

/**
 * @typedef {object} WirelessFormView
 * @property {boolean} available
 * @property {boolean} wirelessReady
 * @property {boolean} radioReady
 * @property {boolean} tcpReady
 * @property {import("./wireless-form.mjs").WirelessStatus | null} status
 * @property {import("./wireless-form.mjs").FormState} form
 * @property {import("./wireless-form.mjs").TestedPair | null} wifiStage
 * @property {number} remainingSeconds
 * @property {string | undefined} selectedSsidHex
 * @property {boolean} keyDirty
 */

/** @brief Render provisioning controls and the temporary-test summary from one snapshot.
 * @param {WirelessFormView} view */
export function renderWirelessForm({
  available, wirelessReady, radioReady, tcpReady, status, form, wifiStage, remainingSeconds, selectedSsidHex, keyDirty,
}) {
  const el = id => queryElement(`#${id}`);
  const option = (id, value) => Array.from(el(id).options ?? []).find(item => item.value === value);
  const working = form.phase === "scanning" || form.phase === "testing";
  const blocked = form.phase !== "idle";
  const tcp = el("direct-settings-tcp-cli");
  tcp.disabled = !available || !tcpReady || working || !status;
  tcp.closest?.("label")?.toggleAttribute("hidden", !available);
  for (const id of ["direct-settings-wifi", "direct-settings-ble"]) {
    el(id).disabled = !available || !radioReady || blocked;
    el(id).closest?.("label")?.toggleAttribute("hidden", !available);
    const configure = option(id, "configure");
    if (configure) configure.disabled = !wirelessReady || blocked;
  }
  const wifiOn = option("direct-settings-wifi", "1");
  if (wifiOn) wifiOn.disabled = Boolean(status && !wifiStage && !status.ssid);
  const bleOn = option("direct-settings-ble", "1");
  if (bleOn) bleOn.disabled = Boolean(status && !keyDirty && !status.ble_key_configured);
  for (const id of ["direct-wifi-scan", "direct-wifi-other", "direct-wifi-connect", "direct-ble-generate",
    "direct-ble-copy"]) {
    el(id).disabled = !wirelessReady || blocked;
  }
  el("direct-wifi-connect").disabled ||= !el("direct-wifi-ssid").value;
  el("direct-wifi-connect").textContent = form.phase === "testing" ? "Testing…" : "Test connection";
  el("direct-wifi-cancel").disabled = working || form.phase === "canceling" || form.cancelNeedsReconnect;
  el("direct-wifi-cancel").textContent = form.phase === "canceling" ? "Clearing…"
    : form.phase === "cancelFailed" ? "Retry clearing test" : "Cancel";
  for (const id of ["direct-wifi-ssid", "direct-wifi-password"]) el(id).disabled = blocked;
  for (const button of el("direct-wifi-networks").children) {
    button.disabled = blocked;
    button.setAttribute("aria-pressed", String(selectedSsidHex !== undefined
      && button.dataset.ssidHex === selectedSsidHex));
  }
  el("direct-ble-copy").disabled ||= !el("direct-ble-secret").value;
  const summary = el("direct-wireless-summary");
  summary.textContent = form.phase === "cancelFailed"
    ? `Could not clear the temporary Wi-Fi test: ${form.cancelError}. ${form.cancelNeedsReconnect
      ? "Reconnect the board before retrying." : "Use Retry clearing test before continuing."}`
    : available && el("direct-settings-wifi").value === "1" && wifiStage?.use_tested_wifi
      ? remainingSeconds === 0 ? "Test credentials expired; test the connection again before saving."
        : `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingSeconds}s.` : "";
  summary.hidden = !summary.textContent;
}
