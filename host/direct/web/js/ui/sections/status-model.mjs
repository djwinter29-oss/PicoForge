import { deriveOperationEligibility } from "../../state/selectors.mjs";

/** Derive display values from a state snapshot without reading or mutating the DOM. */
export function deriveDashboardViewModel(state) {
  const wireless = state.wireless;
  const consoleUrl = wireless?.wifi_enabled && wireless.wifi_state === "connected"
    && wireless.tcp_cli_started && wireless.ipv4 !== "0.0.0.0"
    ? `console.html?${new URLSearchParams({ ipv4: wireless.ipv4 })}` : null;
  const connected = Boolean(state.connected);
  return {
    eligibility: deriveOperationEligibility(state),
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
