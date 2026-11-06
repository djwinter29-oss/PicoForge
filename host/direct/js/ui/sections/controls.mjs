import { CAP } from "../../protocol/constants.mjs";
import { queryElement, setText } from "../dom.mjs";

const element = id => queryElement(`#${id}`);

export function deriveClockOptions(mcu, savedClock) {
  const choices = mcu === "RP2040"
    ? [[0, "Default (125 MHz)"], [150, "150 MHz"], [200, "200 MHz"], [250, "250 MHz"]]
    : mcu === "RP2350" ? [[0, "Default (150 MHz)"], [200, "200 MHz"], [250, "250 MHz"]] : [[0, "Default"]];
  const unsupported = Number.isInteger(savedClock) && savedClock !== 0
    && !choices.some(([value]) => value === savedClock) ? savedClock : null;
  return {
    signature: `${mcu ?? "unknown"}:${unsupported ?? ""}`,
    choices: [...choices, ...(unsupported === null ? [] : [[unsupported, `${unsupported} MHz (saved; unsupported)`, true]])],
  };
}

export function renderClockOptions(state) {
  const model = deriveClockOptions(state.hardware?.mcu, state.settings?.system_clock_mhz);
  const select = element("settings-clock");
  if (select.dataset.clockOptions === model.signature || typeof select.replaceChildren !== "function") return;
  const previous = select.value;
  const options = model.choices.map(([value, label, disabled = false]) => {
    const option = document.createElement("option");
    option.value = String(value);
    option.textContent = label;
    option.disabled = disabled;
    return option;
  });
  select.replaceChildren(...options);
  select.dataset.clockOptions = model.signature;
  select.value = options.some(option => option.value === previous) ? previous : "0";
}

export function deriveControlsViewModel(state, ui, environment) {
  const bluetooth = state.link === "ble";
  const { eligibility } = ui;
  const { blocked, storageReady, settingsReady } = eligibility;
  const bleLinked = bluetooth && (state.connected || state.reconnecting);
  const usbUnsupported = !environment.secure || !environment.hid;
  const bleUnsupported = !environment.secure || !environment.ble;
  const settingsNote = state.settings_message || (!state.connected ? "Connect to read the saved settings."
    : !state.settings_supported ? "Persistent settings save is unavailable."
      : bluetooth && state.msc?.enabled
        ? "USB storage owns the card; settings and reboot require firmware-owned storage."
        : !storageReady ? state.self_test?.active || state.self_test_command_pending
          ? "" : "Waiting for fresh, idle storage status."
          : bluetooth ? "Over Bluetooth, clock, SD Card, TCP CLI, and reboot are available; "
            + "radio and credential provisioning remain USB-only." : "");
  const mscNote = state.reconnecting ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.request_rejected ? "Storage request or transition failed; inspect storage and CDC diagnostics."
      : state.msc?.transition_pending || state.msc_command_pending ? "Waiting for the storage ownership transition result."
        : state.msc?.enabled ? "Host owns storage. Safely eject before return whenever possible." : "";
  return {
    settingsValues: !ui.settingsDirty ? state.settings : null,
    radioHidden: !(state.hardware?.capabilities & CAP.WIRELESS),
    settingsDisabled: !settingsReady || !state.settings_supported,
    disabled: {
      "usb-open": Boolean(state.busy || eligibility.connectionLocked && bluetooth),
      "bluetooth-open": Boolean(state.busy || eligibility.connectionLocked && !bluetooth),
      "usb-connect": Boolean(state.busy || bluetooth || usbUnsupported),
      "bluetooth-scan": Boolean(state.busy || !bluetooth || !bleLinked && bleUnsupported),
      "direct-ble-control-key": Boolean(state.busy || !bluetooth || bleLinked || bleUnsupported),
      "usb-connection-close": Boolean(state.busy),
      "bluetooth-connection-close": Boolean(state.busy),
      "led-control": blocked || !state.runtime?.led_available || !(state.hardware?.capabilities & CAP.LED),
      "msc-control": blocked || !storageReady || bluetooth,
      "settings-save": !settingsReady || !state.settings_supported || !ui.settingsDirty && !ui.wirelessDirty
        || !ui.wirelessCanSave || ui.wirelessDirty && Boolean(state.msc?.enabled),
      "settings-reboot": !settingsReady || Boolean(state.msc?.enabled) || !(state.hardware?.capabilities & CAP.REBOOT),
    },
    text: {
      "usb-connect": !bluetooth && (state.connected || state.reconnecting) ? "Disconnect" : "Select board",
      "bluetooth-scan": bleLinked ? "Disconnect" : "Scan for boards",
      "usb-connection-status": usbUnsupported ? "USB HID requires a supported browser on HTTPS or localhost."
        : !bluetooth && state.error ? state.error : !bluetooth && state.busy ? "Connecting to the selected USB board…"
          : !bluetooth && state.reconnecting ? "Reconnecting to the USB board…"
            : !bluetooth && state.connected ? "Connected through USB." : "Select a PicoForge board to connect.",
      "bluetooth-connection-status": bleUnsupported
        ? "Web Bluetooth requires a supported browser and operating system on HTTPS or localhost."
        : bluetooth && state.error ? state.error : bluetooth && state.busy ? "Select a nearby board; connecting through Bluetooth…"
          : bluetooth && state.connected ? `Connected to ${ui.bleName || state.hardware?.board_name || "PicoForge"}.`
            : "Enter the control key, then scan for nearby PicoForge boards.",
      "msc-control": state.msc?.enabled ? "Return to Pico" : "Enable USB drive",
      "settings-note": settingsNote, "msc-note": mscNote,
    },
    hideStatus: !state.error && Boolean(settingsNote),
  };
}

export function renderControls(state, ui) {
  const model = deriveControlsViewModel(state, ui, {
    secure: globalThis.isSecureContext, hid: Boolean(navigator.hid), ble: Boolean(navigator.bluetooth),
  });
  if (model.settingsValues) {
    element("settings-clock").value = String(model.settingsValues.system_clock_mhz);
    element("settings-storage").value = String(model.settingsValues.sdcard_profile);
    ["direct-settings-wifi", "direct-settings-ble"].forEach((id, bit) => {
      element(id).value = String(model.settingsValues.radios >> bit & 1);
    });
  }
  setText(model.text);
  for (const id of ["settings-clock", "settings-storage", "direct-settings-wifi", "direct-settings-ble"]) {
    element(id).disabled = model.settingsDisabled;
  }
  for (const id of ["direct-settings-wifi", "direct-settings-ble"]) {
    queryElement(`label[for="${id}"]`).hidden = model.radioHidden;
  }
  for (const [id, disabled] of Object.entries(model.disabled)) element(id).disabled = disabled;
  element("settings-note").hidden = !model.text["settings-note"];
  element("msc-note").hidden = !model.text["msc-note"];
  element("status-message").hidden = model.hideStatus;
}
