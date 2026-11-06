import { deriveOperationEligibility } from "../../state/selectors.mjs";

/**
 * @typedef {Omit<import("../../state/reducer.mjs").DashboardState,
 *   "error" | "recovery_message" | "wireless_error" | "wireless_poll_error"> & {
 *   error?: string | null, recovery_message?: string | null, wireless_error?: string | null,
 *   wireless_poll_error?: string | null}} DashboardPresentation
 */

/** @brief Readiness with the dashboard-only wireless-form lock folded in: a running form operation blocks writes. */
export function deriveViewEligibility(state, formBusy = false) {
  const eligibility = deriveOperationEligibility(state);
  return formBusy ? { ...eligibility, blocked: true, settingsReady: false, wirelessReady: false,
    selfTestReady: false } : eligibility;
}

/**
 * @brief Derive display values from a snapshot without reading or mutating the DOM.
 * @param {DashboardPresentation} state
 * @param {boolean} [formBusy]
 */
export function deriveDashboardViewModel(state, formBusy = false) {
  const wireless = state.wireless;
  const consoleUrl = wireless?.wifi_enabled && wireless.wifi_state === "connected"
    && wireless.tcp_cli_started && wireless.ipv4 !== "0.0.0.0"
    ? `console.html?${new URLSearchParams({ ipv4: wireless.ipv4 })}` : null;
  const connected = Boolean(state.connected);
  return {
    eligibility: deriveViewEligibility(state, formBusy),
    console_url: consoleUrl,
    connection_state: state.error ? "error" : connected ? "connected" : "disconnected",
    connection_label: state.error ? "Attention" : state.reconnecting ? "Reconnecting"
      : connected ? "Connected" : "Disconnected",
    status_message: state.error ?? (state.reconnecting ? "USB device reconnecting; reopening HID automatically"
      : state.recovery_message ?? (state.reboot_pending ? "Reboot accepted; waiting for USB reconnection"
        : state.runtime ? "HID status is live"
          : connected ? "Waiting for first status report" : "Select a PicoForge device")),
  };
}
