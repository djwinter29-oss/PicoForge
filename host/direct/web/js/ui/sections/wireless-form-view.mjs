// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** @file Wireless form presentation: apply a readiness/draft snapshot without performing device I/O. */
import { queryElement, setText } from "../dom.mjs";

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
 * @property {string | null} saveBlockReason
 */

/** @brief Render provisioning controls and the temporary-test summary from one snapshot.
 * @param {WirelessFormView} view */
export function renderWirelessForm({
  available, wirelessReady, radioReady, tcpReady, status, form, wifiStage, remainingSeconds, selectedSsidHex, keyDirty,
  saveBlockReason,
}) {
  /** @param {string} id */
  const el = id => queryElement(`#${id}`);
  /** @param {string} id */
  const select = id => queryElement(`select#${id}`);
  /** @param {string} id @param {string} value @return {HTMLOptionElement | undefined} */
  const option = (id, value) => Array.from(select(id).options).find(item => item.value === value);
  const working = form.phase === "scanning" || form.phase === "testing";
  const blocked = form.phase !== "idle";
  const tcp = select("direct-settings-tcp-cli");
  tcp.disabled = !available || !tcpReady || working || !status;
  tcp.closest?.("label")?.toggleAttribute("hidden", !available);
  for (const id of ["direct-settings-wifi", "direct-settings-ble"]) {
    select(id).disabled = !available || !radioReady || blocked;
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
    queryElement(`button#${id}`).disabled = !wirelessReady || blocked;
  }
  queryElement("button#direct-wifi-connect").disabled ||= !queryElement("input#direct-wifi-ssid").value;
  el("direct-wifi-connect").textContent = form.phase === "testing" ? "Testing…" : "Test connection";
  queryElement("button#direct-wifi-cancel").disabled = working || form.phase === "canceling" || form.cancelNeedsReconnect;
  el("direct-wifi-cancel").textContent = form.phase === "canceling" ? "Clearing…"
    : form.phase === "cancelFailed" ? "Retry clearing test" : "Cancel";
  for (const id of ["direct-wifi-ssid", "direct-wifi-password"]) queryElement(`input#${id}`).disabled = blocked;
  for (const button of Array.from(el("direct-wifi-networks").querySelectorAll("button"))) {
    button.disabled = blocked;
    button.setAttribute("aria-pressed", String(selectedSsidHex !== undefined
      && button.dataset.ssidHex === selectedSsidHex));
  }
  queryElement("button#direct-ble-copy").disabled ||= !queryElement("input#direct-ble-secret").value;
  const summary = el("direct-wireless-summary");
  const summaryText = form.phase === "cancelFailed"
    ? `Could not clear the temporary Wi-Fi test: ${form.cancelError}. ${form.cancelNeedsReconnect
      ? "Reconnect the board before retrying." : "Use Retry clearing test before continuing."}`
    : available ? saveBlockReason ?? (select("direct-settings-wifi").value === "1" && wifiStage?.use_tested_wifi
      ? `Wi-Fi “${wifiStage.ssid}” tested; expires in ${remainingSeconds}s.` : "") : "";
  setText({ "direct-wireless-summary": summaryText });
  summary.hidden = !summaryText;
}
