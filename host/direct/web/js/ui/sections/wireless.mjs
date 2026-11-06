// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { CAP } from "../../protocol/constants.mjs";
import { queryElement } from "../dom.mjs";

export function deriveWirelessViewModel(state) {
  const wireless = state.wireless;
  const unavailable = !state.connected ? "--" : state.wireless_error ? "Unavailable" : !wireless ? "--" : null;
  const name = state.hardware?.serial_number ? `PicoForge-${state.hardware.serial_number}` : "PicoForge";
  const wifi = unavailable ?? (!wireless.wifi_enabled || !wireless.ssid ? "Disabled"
    : wireless.wifi_state === "connected" ? `${wireless.ssid} (${wireless.ipv4})`
      : `${wireless.ssid} (${wireless.wifi_state === "connecting" ? "connecting" : "disconnected"})`);
  const ble = unavailable ?? (!wireless.ble_enabled ? "Disabled"
    : !wireless.ble_key_configured && wireless.ble_state === "off" ? "Off (no control key)"
      : `${name} (${wireless.ble_state === "connected" ? "connected"
        : wireless.ble_state === "advertising" ? "advertising" : "starting"})`);
  return {
    hidden: !(state.hardware?.capabilities & CAP.WIRELESS),
    wifi, ble,
    wifiTitle: state.wireless_error ?? state.wireless_poll_error ?? (state.hardware?.serial_number
      ? `picoforge-${state.hardware.serial_number}.local` : ""),
    bleTitle: state.wireless_error ?? state.wireless_poll_error ?? "",
    consoleUrl: !unavailable && wireless?.wifi_enabled && wireless.wifi_state === "connected"
      && wireless.tcp_cli_started ? state.console_url : null,
  };
}

export function renderWirelessStatus(state) {
  const model = deriveWirelessViewModel(state);
  for (const name of ["wifi", "ble"]) {
    queryElement(`#direct-${name}-diagnostics`).hidden = model.hidden;
    const status = queryElement(`#direct-${name}-state`);
    if (name === "wifi" && model.consoleUrl) {
      const existing = status.querySelector?.("a");
      const link = existing ?? document.createElement("a");
      link.textContent = model.wifi;
      link.href = model.consoleUrl;
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      link.title = "Open the Wi-Fi CLI console in a new tab";
      link.onclick = event => state.open_console?.(event, model.consoleUrl);
      if (!existing) status.replaceChildren(link);
    } else status.textContent = model[name];
    status.title = model[`${name}Title`];
  }
}
