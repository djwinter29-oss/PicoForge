import { PicoForgeWebHid, withTimeout } from "./device.mjs?v=7";
import { renderStatus } from "./view.mjs?v=16";
import { PicoForgeBluetooth } from "./bluetooth.mjs?v=10";
import { bindWirelessForm } from "./wireless-form.mjs?v=18";
import { deriveOperationEligibility } from "./eligibility.mjs?v=1";
import {
  cancelWireless, decodeWireless, registerWirelessRecovery, saveWireless, scanWireless, testWireless,
} from "./wireless.mjs?v=10";

const element = id => document.querySelector(`#${id}`);
const usbDialog = element("usb-connection-dialog");
const bluetoothDialog = element("bluetooth-connection-dialog");
const ledControl = element("led-control");
const mscControl = element("msc-control");
const settingsSave = element("settings-save");
const settingsReboot = element("settings-reboot");
const settingsClock = element("settings-clock");
const settingsStorage = element("settings-storage");
const settingsRadios = [element("settings-wifi"), element("settings-ble")];
const selectedRadios = () => client.state.hardware?.capabilities & 128
  ? Number(settingsRadios[0].value) | Number(settingsRadios[1].value) << 1 : 0;
const configurationDialog = element("configuration-dialog");
const rebootDialog = element("reboot-dialog");
const returnDialog = element("return-dialog");
const shareDialog = element("share-dialog");
let settingsDirty = false;
let configurationForceSave = false;
let client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);
registerWirelessRecovery(client);
const wirelessForm = bindWirelessForm({
  supported: () => Boolean(client.state.hardware?.capabilities & 128),
  ready: () => client.wirelessReady(),
  load: async () => client.state.wireless ?? decodeWireless(await client.device.receiveFeatureReport(30)),
  scan: () => scanWireless(client),
  test: (settings, onProgress) => testWireless(client, settings, onProgress),
  cancelTest: () => cancelWireless(client),
  changed: () => render(client.state),
});
let wirelessLoadedAt = 0;
let wirelessReading = false;

async function refreshWirelessStatus() {
  const target = client;
  const device = target.device;
  const generation = target.generation;
  wirelessReading = true;
  wirelessLoadedAt = Date.now();
  try {
    const wireless = decodeWireless(await withTimeout(device.receiveFeatureReport(30), 1500));
    if (target === client && device === target.device && generation === target.generation) {
      target.update({ wireless, wireless_error: null });
    }
  } catch (error) {
    if (target === client && device === target.device && generation === target.generation) {
      target.update({ wireless: null, wireless_error: `Wi-Fi status read failed: ${error.message}` });
    }
  } finally { wirelessReading = false; }
}

function render(state) {
  const wireless = state.wireless;
  // View-only Bluetooth shows the network but no console link; older firmware also zeroes the IPv4 over Bluetooth.
  const consoleAllowed = !(client instanceof PicoForgeBluetooth) || client.accessMode === "control";
  renderStatus({ ...state, console_url: consoleAllowed && wireless?.wifi_enabled
    && wireless.wifi_state === "connected" && wireless.tcp_cli_started && wireless.ipv4 !== "0.0.0.0"
    ? `console.html?ipv4=${wireless.ipv4}&auth=${Number(wireless.tcp_cli_auth_required)}` : null });
  if (!state.connected) {
    wirelessLoadedAt = 0;
    wirelessForm.reset();
    settingsDirty = false;
    for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) dialog.close();
  }
  if (!settingsDirty && state.settings) {
    settingsClock.value = String(state.settings.system_clock_mhz);
    settingsStorage.value = String(state.settings.sdcard_profile);
    settingsRadios.forEach((control, bit) => { control.value = String(state.settings.radios >> bit & 1); });
  }
  const bluetooth = state.transport === "ble";
  const bleClient = client instanceof PicoForgeBluetooth;
  const eligibility = deriveOperationEligibility(state, {
    bluetooth,
    bleClient,
    accessMode: bleClient ? client.accessMode : null,
    outstanding: client.outstanding,
    wirelessBusy: wirelessForm.operationBusy(),
  });
  const { blocked, storageReady, settingsReady } = eligibility;
  element("usb-open").disabled = state.busy || Boolean(eligibility.connectionLocked && bleClient);
  element("bluetooth-open").disabled = state.busy || Boolean(eligibility.connectionLocked && !bleClient);
  element("usb-connect").textContent = !bleClient && (state.connected || state.reconnecting)
    ? "Disconnect" : "Select board";
  element("usb-connect").disabled = state.busy || bleClient || !globalThis.isSecureContext || !navigator.hid;
  element("bluetooth-scan").disabled = state.busy || !bleClient || state.connected
    || !globalThis.isSecureContext || !navigator.bluetooth;
  element("bluetooth-disconnect").disabled = state.busy || !bleClient || !state.connected;
  element("bluetooth-access-step").hidden = !bleClient || !state.connected;
  element("ble-view-only").disabled = state.busy || !bleClient || !state.connected;
  element("ble-authorize").disabled = state.busy || !bleClient || !state.connected;
  element("ble-control-key").disabled = element("ble-authorize").disabled;
  const bleAccessStatus = element("ble-access-status");
  bleAccessStatus.hidden = !bleClient || !state.connected;
  bleAccessStatus.textContent = !state.connected ? ""
    : client.accessMode === "control" ? "Bluetooth access: control mode; the board checks the key on every command."
      : client.accessMode === "view-only" ? "Bluetooth access: view only; only the board LED can be switched."
        : "Bluetooth access: choose View only or enter the control key.";
  element("usb-connection-close").disabled = state.busy;
  element("bluetooth-connection-close").disabled = state.busy || eligibility.bleAccessChoicePending;
  element("usb-connection-status").textContent = !globalThis.isSecureContext || !navigator.hid
    ? "USB HID requires a supported browser on HTTPS or localhost."
    : !bleClient && state.error ? state.error
    : !bleClient && state.busy ? "Connecting to the selected USB board…"
    : !bleClient && state.reconnecting ? "Reconnecting to the USB board…"
    : !bleClient && state.connected ? "Connected through USB." : "Select a PicoForge board to connect.";
  element("bluetooth-connection-status").textContent = !globalThis.isSecureContext || !navigator.bluetooth
    ? "Web Bluetooth requires a supported browser and operating system on HTTPS or localhost."
    : bleClient && state.error ? state.error
    : bleClient && state.busy ? (state.connected ? "Loading control key…"
      : "Select a nearby board; connecting through Bluetooth…")
    : bleClient && state.connected
      ? `Connected to ${client.bleDevice?.name || state.hardware?.board_name || "PicoForge"}. `
        + (client.accessMode === "control" ? "Control mode selected."
          : client.accessMode === "view-only" ? "View-only diagnostics."
            : "Choose View only or enter the control key to enable controls.")
      : "Scan for nearby PicoForge boards.";
  if (!state.connected) element("ble-control-key").value = "";
  ledControl.disabled = blocked || !state.board_status?.led_available || !(state.hardware?.capabilities & 1);
  if (bluetooth && client.accessMode !== "control") {
    ledControl.disabled ||= client.accessMode === null;
    element("self-test-start").disabled = true;
    element("self-test-cancel").disabled = true;
  }
  mscControl.disabled = blocked || !storageReady || bluetooth;
  mscControl.textContent = state.msc?.enabled ? "Return to Pico" : "Enable USB drive";
  for (const id of ["settings-wifi", "settings-ble"]) {
    document.querySelector(`label[for="${id}"]`).hidden = !(state.hardware?.capabilities & 128);
  }
  for (const control of [settingsClock, settingsStorage, ...settingsRadios, settingsSave]) {
    control.disabled = !settingsReady || !state.settings_supported;
  }
  settingsSave.disabled ||= !settingsDirty && !wirelessForm.dirty();
  settingsSave.disabled ||= !wirelessForm.canSave();
  // Credentials have no forced-save path: provisioning requires firmware-owned storage.
  settingsSave.disabled ||= wirelessForm.dirty() && Boolean(state.msc?.enabled);
  settingsSave.disabled ||= Boolean(state.msc?.enabled && !(state.hardware?.capabilities & 64));
  settingsReboot.disabled = !settingsReady || state.msc?.enabled || !(state.hardware?.capabilities & 2);
  element("settings-note").textContent = state.settings_message
    || (!state.connected ? "Connect to read the saved settings."
      : !state.settings_supported ? "Persistent settings save is unavailable."
        : !storageReady ? (state.self_test?.active || state.self_test_command_pending
          ? "" : "Waiting for fresh, idle storage status.")
          : state.msc?.enabled && !(state.hardware?.capabilities & 64)
            ? "Update firmware or return storage to Pico before saving settings."
            : "");
  element("settings-note").hidden = !element("settings-note").textContent;
  element("status-message").hidden = !state.error && !element("settings-note").hidden;
  element("msc-note").textContent = state.reconnecting
    ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.request_rejected ? "Storage request or transition failed; inspect storage and CDC diagnostics."
      : state.msc?.transition_pending || state.msc_command_pending
        ? "Waiting for the storage ownership transition result."
        : state.msc?.enabled ? "Host owns storage. Safely eject before return whenever possible." : "";
  element("msc-note").hidden = !element("msc-note").textContent;
  wirelessForm.update(state);
  if (state.connected && state.wireless && storageReady && !state.busy && !bluetooth) {
    void wirelessForm.load();
  }
  if (Date.now() - wirelessLoadedAt >= 2000 && state.connected && !state.busy && !state.reboot_pending
    && !client.outstanding && !wirelessForm.operationBusy() && !wirelessReading
    && (state.hardware?.capabilities & 128)) void refreshWirelessStatus();
}

async function run(operation) {
  try { await operation(); } catch (error) { client.update({ error: error.message }); }
}

for (const [id, dialog, bluetooth] of [["usb-open", usbDialog, false],
  ["bluetooth-open", bluetoothDialog, true]]) {
  element(id).addEventListener("click", () => {
    if (element(id).disabled) return;
    if ((client instanceof PicoForgeBluetooth) !== bluetooth) {
      client = bluetooth ? new PicoForgeBluetooth(globalThis.isSecureContext ? navigator.bluetooth : undefined, render)
        : new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);
      registerWirelessRecovery(client);
    }
    render(client.state);
    dialog.showModal();
  });
}
element("usb-connect").addEventListener("click", () => run(async () => {
  if (element("usb-connect").disabled) return;
  if (client.state.connected || client.state.reconnecting) await client.disconnect();
  else {
    await client.connect();
    if (client.state.connected) usbDialog.close();
  }
}));
element("bluetooth-scan").addEventListener("click", () => run(async () => {
  if (element("bluetooth-scan").disabled) return;
  await client.connect();
  if (client.state.connected) element("ble-view-only").focus();
}));
element("bluetooth-disconnect").addEventListener("click", () => run(async () => {
  if (!element("bluetooth-disconnect").disabled) await client.disconnect();
}));
element("ble-view-only").addEventListener("click", () => run(async () => {
  if (element("ble-view-only").disabled) return;
  client.setViewOnly();
  bluetoothDialog.close();
}));
element("ble-authorize").addEventListener("click", () => run(async () => {
  if (element("ble-authorize").disabled) return;
  const target = client;
  const generation = target.generation;
  target.update({ busy: true, error: null });
  try {
    await target.setControlKey(element("ble-control-key").value);
    if (target !== client || generation !== target.generation || !target.state.connected) {
      target.bleKey = null;
      target.accessMode = null;
      throw new Error("Bluetooth disconnected while loading the control key. Scan and connect again.");
    }
    target.update({ error: null });
    bluetoothDialog.close();
  }
  finally {
    element("ble-control-key").value = "";
    if (target === client) target.update({ busy: false });
  }
}));
for (const [name, dialog] of [["usb", usbDialog], ["bluetooth", bluetoothDialog]]) {
  element(`${name}-connection-close`).addEventListener("click", () => {
    if (!client.state.busy) dialog.close();
  });
  dialog.addEventListener("cancel", event => {
    const bleAccessChoicePending = name === "bluetooth" && client instanceof PicoForgeBluetooth
      && client.state.connected && client.accessMode === null;
    if (client.state.busy || bleAccessChoicePending) event.preventDefault();
  });
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault();
      const bleAccessChoicePending = name === "bluetooth" && client instanceof PicoForgeBluetooth
        && client.state.connected && client.accessMode === null;
      if (!client.state.busy && !bleAccessChoicePending) dialog.close();
    }
  });
  dialog.addEventListener("close", () => { element("ble-control-key").value = ""; });
}
ledControl.addEventListener("click", () => run(() => client.setLed(!client.state.board_status?.led_on)));
mscControl.addEventListener("click", () => run(async () => {
  if (mscControl.disabled) return;
  const enabled = client.state.msc?.enabled;
  if (enabled) returnDialog.showModal();
  else shareDialog.showModal();
}));
element("self-test-start").addEventListener("click", () => run(() =>
  client.setSelfTest("start", element("self-test-size").valueAsNumber)));
element("self-test-cancel").addEventListener("click", () => run(() => client.setSelfTest("cancel")));
element("self-test-size").addEventListener("input", () => render(client.state));
for (const control of [settingsClock, settingsStorage, ...settingsRadios]) {
  control.addEventListener("change", () => {
    settingsDirty = Number(settingsClock.value) !== client.state.settings?.system_clock_mhz
      || Number(settingsStorage.value) !== client.state.settings?.sdcard_profile
      || selectedRadios() !== (client.state.settings?.radios ?? 0);
    render(client.state);
  });
}
settingsSave.addEventListener("click", () => {
  if (settingsSave.disabled) return;
  configurationForceSave = Boolean(client.state.msc?.enabled);
  element("configuration-force-warning").hidden = !configurationForceSave;
  element("configuration-wireless-warning").hidden = !wirelessForm.dirty();
  const clock = Number(settingsClock.value);
  element("configuration-dialog-profile").textContent =
    `System clock: ${clock ? `${clock} MHz` : "Default"}. `
    + `SD Card: ${Number(settingsStorage.value) === 1 ? "High-Speed" : "Standard"}.`
    + (client.state.hardware?.capabilities & 128 ? ` Wi-Fi: ${selectedRadios() & 1 ? "Enabled" : "Disabled"}.`
      + ` Bluetooth: ${selectedRadios() & 2 ? "Enabled" : "Disabled"}.` : "");
  configurationDialog.showModal();
});
element("confirm-configuration").addEventListener("click", () => run(async () => {
  if (!configurationDialog.open) return;
  configurationDialog.close();
  if (settingsSave.disabled) return;
  if (Boolean(client.state.msc?.enabled) !== configurationForceSave) return;
  const settings = { system_clock_mhz: Number(settingsClock.value), sdcard_profile: Number(settingsStorage.value),
    radios: selectedRadios() };
  if (!wirelessForm.dirty()) {
    if (await client.saveSettings(settings, configurationForceSave)) settingsDirty = false;
  } else {
    try {
      wirelessForm.finish(await saveWireless(client, { ...wirelessForm.request(),
        wifi_enabled: Boolean(settings.radios & 1), ble_enabled: Boolean(settings.radios & 2), board: settings }));
      settingsDirty = false;
      client.update({ settings_message: "Settings saved. Waiting for the board to reboot." });
    } catch (error) {
      wirelessForm.finish(null, error.message);
      client.update({ settings_message: `Save failed: ${error.message}` });
    }
  }
  render(client.state);
}));
settingsReboot.addEventListener("click", () => {
  if (settingsReboot.disabled) return;
  element("reboot-consequence").textContent = settingsDirty || wirelessForm.dirty()
    ? "Unsaved dashboard changes will be lost." : "Saved next-boot settings will be applied.";
  rebootDialog.showModal();
});
element("confirm-reboot").addEventListener("click", () => run(async () => {
  if (!rebootDialog.open) return;
  rebootDialog.close();
  if (!settingsReboot.disabled) await client.reboot(true);
}));
element("confirm-return").addEventListener("click", () => run(async () => {
  if (!returnDialog.open) return;
  returnDialog.close();
  if (!mscControl.disabled && client.state.msc?.enabled) await client.setMsc(false, true);
}));
element("confirm-share").addEventListener("click", () => run(async () => {
  if (!shareDialog.open) return;
  shareDialog.close();
  if (!mscControl.disabled && !client.state.msc?.enabled) await client.setMsc(true, false);
}));
for (const [name, dialog] of [["configuration", configurationDialog], ["reboot", rebootDialog],
  ["return", returnDialog], ["share", shareDialog]]) {
  element(`cancel-${name}`).addEventListener("click", () => dialog.close());
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault();
      dialog.close();
    }
  });
}
render(client.state);
