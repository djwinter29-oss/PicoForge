import { PicoForgeWebHid, withTimeout } from "./device.mjs?v=11";
import { renderStatus } from "./view.mjs?v=19";
import { PicoForgeBluetooth } from "./bluetooth.mjs?v=14";
import {
  bytesToHex, CONSOLE_AUTH_CHANNEL, CONSOLE_AUTH_ERROR, CONSOLE_AUTH_REQUEST, CONSOLE_AUTH_RESPONSE,
  parseHexBytes, randomHex,
} from "./console-auth.mjs?v=1";
import { bindWirelessForm } from "./wireless-form.mjs?v=21";
import { deriveOperationEligibility } from "./eligibility.mjs?v=3";
import {
  cancelWireless, decodeWireless, registerWirelessRecovery, saveWireless, scanWireless, testWireless,
} from "./wireless.mjs?v=13";

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
  loadReady: () => client.state.transport === "ble"
    ? operationEligibility(client.state).settingsReady : client.wirelessReady(),
  tcpReady: () => client.state.transport === "ble"
    ? operationEligibility(client.state).settingsReady
    : client.wirelessReady() && Number(settingsRadios[0].value) === 1,
  load: async () => client.state.wireless ?? decodeWireless(await client.device.receiveFeatureReport(30)),
  scan: () => scanWireless(client),
  test: (settings, onProgress) => testWireless(client, settings, onProgress),
  cancelTest: () => cancelWireless(client),
  changed: () => render(client.state),
});
const operationEligibility = (state, wirelessBusy = false) => deriveOperationEligibility(state, {
  bluetooth: state.transport === "ble",
  outstanding: client.outstanding,
  wirelessBusy,
});
let wirelessLoadedAt = 0;
let wirelessReading = false;
let consoleAuthToken = null;

const consoleAuthChannel = typeof window !== "undefined" && typeof window.BroadcastChannel === "function"
  ? new window.BroadcastChannel(CONSOLE_AUTH_CHANNEL) : null;

async function provideConsoleProof(request) {
  if (!consoleAuthToken || request?.type !== CONSOLE_AUTH_REQUEST || request.auth_token !== consoleAuthToken
    || typeof request.request_id !== "string" || !/^[a-f0-9]{32}$/i.test(request.request_id)) return;
  const response = { auth_token: request.auth_token, request_id: request.request_id };
  try {
    const nonce = parseHexBytes(request.nonce, 16, "TCP CLI nonce");
    const uid = parseHexBytes(request.uid, 8, "TCP CLI board identity");
    const target = client;
    if (!target.state.connected || !target.state.hardware) {
      throw new Error("Connect this board to the Direct dashboard over USB or Bluetooth first");
    }
    const boardUid = parseHexBytes(target.state.hardware.serial_number, 8, "connected board identity");
    if (uid.some((byte, index) => byte !== boardUid[index])) {
      throw new Error("The Wi-Fi console challenge belongs to a different board");
    }
    const proof = await target.tcpCliAuthProof(nonce, uid);
    if (target !== client || !target.state.connected) {
      throw new Error("The dashboard disconnected while authorizing the Wi-Fi console");
    }
    if (!(proof instanceof Uint8Array) || proof.length !== 16) {
      throw new Error("The board returned an invalid TCP CLI proof");
    }
    consoleAuthChannel?.postMessage({ ...response, type: CONSOLE_AUTH_RESPONSE,
      uid: request.uid.toLowerCase(), proof: bytesToHex(proof) });
  } catch (error) {
    consoleAuthChannel?.postMessage({ ...response, type: CONSOLE_AUTH_ERROR,
      error: error instanceof Error ? error.message : "Could not authorize the Wi-Fi console" });
  }
}

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
  let consoleUrl = null;
  if (wireless?.wifi_enabled && wireless.wifi_state === "connected" && wireless.tcp_cli_started
    && wireless.ipv4 !== "0.0.0.0") {
    const authRequired = Boolean(wireless.tcp_cli_auth_required);
    const query = new URLSearchParams({ ipv4: wireless.ipv4, auth: Number(authRequired).toString() });
    if (authRequired) {
      consoleAuthToken ??= randomHex();
      query.set("auth_token", consoleAuthToken);
    }
    consoleUrl = `console.html?${query}`;
  }
  renderStatus({ ...state, console_url: consoleUrl });
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
  const eligibility = operationEligibility(state, wirelessForm.operationBusy());
  const { blocked, storageReady, settingsReady } = eligibility;
  element("usb-open").disabled = state.busy || Boolean(eligibility.connectionLocked && bleClient);
  element("bluetooth-open").disabled = state.busy || Boolean(eligibility.connectionLocked && !bleClient);
  element("usb-connect").textContent = !bleClient && (state.connected || state.reconnecting)
    ? "Disconnect" : "Select board";
  element("usb-connect").disabled = state.busy || bleClient || !globalThis.isSecureContext || !navigator.hid;
  const bleLinked = bleClient && (state.connected || state.reconnecting);
  const bleUnsupported = !globalThis.isSecureContext || !navigator.bluetooth;
  element("bluetooth-scan").textContent = bleLinked ? "Disconnect" : "Scan for boards";
  element("bluetooth-scan").disabled = state.busy || !bleClient || (!bleLinked && bleUnsupported);
  element("ble-control-key").disabled = state.busy || !bleClient || bleLinked || bleUnsupported;
  element("usb-connection-close").disabled = state.busy;
  element("bluetooth-connection-close").disabled = state.busy;
  element("usb-connection-status").textContent = !globalThis.isSecureContext || !navigator.hid
    ? "USB HID requires a supported browser on HTTPS or localhost."
    : !bleClient && state.error ? state.error
    : !bleClient && state.busy ? "Connecting to the selected USB board…"
    : !bleClient && state.reconnecting ? "Reconnecting to the USB board…"
    : !bleClient && state.connected ? "Connected through USB." : "Select a PicoForge board to connect.";
  element("bluetooth-connection-status").textContent = !globalThis.isSecureContext || !navigator.bluetooth
    ? "Web Bluetooth requires a supported browser and operating system on HTTPS or localhost."
    : bleClient && state.error ? state.error
    : bleClient && state.busy ? "Select a nearby board; connecting through Bluetooth…"
    : bleClient && state.connected
      ? `Connected to ${client.bleDevice?.name || state.hardware?.board_name || "PicoForge"}.`
      : "Enter the control key, then scan for nearby PicoForge boards.";
  ledControl.disabled = blocked || !state.board_status?.led_available || !(state.hardware?.capabilities & 1);
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
  settingsReboot.disabled = !settingsReady || state.msc?.enabled || !(state.hardware?.capabilities & 2);
  element("settings-note").textContent = state.settings_message
    || (!state.connected ? "Connect to read the saved settings."
      : !state.settings_supported ? "Persistent settings save is unavailable."
        : bluetooth && state.msc?.enabled ? "USB storage owns the card; settings and reboot require firmware-owned storage."
        : !storageReady ? (state.self_test?.active || state.self_test_command_pending
          ? "" : "Waiting for fresh, idle storage status.")
          : bluetooth ? "Over Bluetooth, clock, SD Card, TCP CLI, and reboot are available; radio and credential provisioning remain USB-only."
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
  if (bluetooth) settingsRadios.forEach(control => { control.disabled = true; });
  if (state.connected && storageReady && !state.busy) {
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
  if (client.state.connected || client.state.reconnecting) {
    await client.disconnect();
    return;
  }
  const key = element("ble-control-key").value;
  element("ble-control-key").value = "";
  await client.connect(key);
  if (client.state.connected) bluetoothDialog.close();
}));
for (const [name, dialog] of [["usb", usbDialog], ["bluetooth", bluetoothDialog]]) {
  element(`${name}-connection-close`).addEventListener("click", () => {
    if (!client.state.busy) dialog.close();
  });
  dialog.addEventListener("cancel", event => {
    if (client.state.busy) event.preventDefault();
  });
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault();
      if (!client.state.busy) dialog.close();
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
  const bluetooth = client.state.transport === "ble";
  configurationForceSave = Boolean(client.state.msc?.enabled);
  element("configuration-force-warning").hidden = !configurationForceSave;
  const wirelessWarning = element("configuration-wireless-warning");
  wirelessWarning.textContent = bluetooth
    ? "This saves the TCP CLI switch without sending Wi-Fi credentials or the Control key. Bluetooth is authenticated but not encrypted."
    : "Wi-Fi and Bluetooth credentials are saved with these settings. Keep a copy of the Control key; it cannot be read back. Flash credentials are not encrypted.";
  wirelessWarning.hidden = !wirelessForm.dirty();
  const clock = Number(settingsClock.value);
  element("configuration-dialog-profile").textContent =
    `System clock: ${clock ? `${clock} MHz` : "Default"}. `
    + `SD Card: ${Number(settingsStorage.value) === 1 ? "High-Speed" : "Standard"}.`
    + (!bluetooth && client.state.hardware?.capabilities & 128
      ? ` Wi-Fi: ${selectedRadios() & 1 ? "Enabled" : "Disabled"}.`
        + ` Bluetooth: ${selectedRadios() & 2 ? "Enabled" : "Disabled"}.` : "")
    + (bluetooth && wirelessForm.dirty()
      ? ` TCP CLI: ${element("settings-tcp-cli").value === "1" ? "Enabled" : "Disabled"}.` : "");
  configurationDialog.showModal();
});
element("confirm-configuration").addEventListener("click", () => run(async () => {
  if (!configurationDialog.open) return;
  configurationDialog.close();
  if (settingsSave.disabled) return;
  if (Boolean(client.state.msc?.enabled) !== configurationForceSave) return;
  const bluetooth = client.state.transport === "ble";
  const tcpCliEnabled = bluetooth && wirelessForm.dirty()
    ? element("settings-tcp-cli").value === "1" : undefined;
  const settings = { system_clock_mhz: Number(settingsClock.value), sdcard_profile: Number(settingsStorage.value),
    radios: bluetooth ? client.state.settings?.radios ?? 0 : selectedRadios() };
  if (bluetooth || !wirelessForm.dirty()) {
    if (await client.saveSettings(settings, configurationForceSave, tcpCliEnabled)) {
      if (tcpCliEnabled !== undefined) wirelessForm.finish({ tcp_cli_enabled: tcpCliEnabled });
      settingsDirty = false;
    }
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
if (consoleAuthChannel) {
  consoleAuthChannel.onmessage = event => { void provideConsoleProof(event.data); };
  window.addEventListener?.("pagehide", () => consoleAuthChannel.close(), { once: true });
}
render(client.state);
