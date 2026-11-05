import { CAP, PicoForgeWebHid } from "./device.mjs?v=47";
import { renderControls, renderStatus } from "./view.mjs?v=47";
import { PicoForgeBluetooth } from "./bluetooth.mjs?v=47";
import {
  bytesToHex, CONSOLE_AUTH_CHANNEL, CONSOLE_AUTH_ERROR, CONSOLE_AUTH_REQUEST, CONSOLE_AUTH_RESPONSE,
  parseHexBytes, randomHex,
} from "./console-auth.mjs?v=47";
import { bindWirelessForm } from "./wireless-form.mjs?v=47";
import { deriveOperationEligibility } from "./eligibility.mjs?v=47";
import {
  cancelWireless, readWireless, saveWireless, scanWireless, testWireless, wirelessPollInterval,
} from "./wireless.mjs?v=47";

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
const selectedRadios = () => client.state.hardware?.capabilities & CAP.WIRELESS
  ? Number(settingsRadios[0].value) | Number(settingsRadios[1].value) << 1 : 0;
const configurationDialog = element("configuration-dialog");
const rebootDialog = element("reboot-dialog");
const returnDialog = element("return-dialog");
const shareDialog = element("share-dialog");
let settingsDirty = false;
let configurationForceSave = false;
let refreshQueued = false;
/** Coalesce bursts of client state changes into one render and side-effect pass per microtask. */
const scheduleRefresh = () => {
  if (refreshQueued) return;
  refreshQueued = true;
  queueMicrotask(refresh);
};
let client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, scheduleRefresh);
const bluetoothLink = () => client.state.link === "ble";
const wirelessForm = bindWirelessForm({
  supported: () => Boolean(client.state.hardware?.capabilities & CAP.WIRELESS),
  ready: () => client.eligibility().wirelessReady,
  loadReady: () => client.eligibility()[bluetoothLink() ? "settingsReady" : "wirelessReady"],
  tcpReady: () => bluetoothLink() ? client.eligibility().settingsReady
    : client.eligibility().wirelessReady && Number(settingsRadios[0].value) === 1,
  load: async () => client.state.wireless ?? readWireless(client),
  scan: () => scanWireless(client),
  test: (settings, onProgress) => testWireless(client, settings, onProgress),
  cancelTest: () => cancelWireless(client),
  changed: scheduleRefresh,
});
// Pure snapshot rule for rendering; client.eligibility() also expires stale reports before a write.
const operationEligibility = state => deriveOperationEligibility(state, {
  outstanding: client.outstanding ?? client.pendingWirelessCommand,
  wirelessBusy: Boolean(client.wirelessOperation) || wirelessForm.operationBusy(),
});
let wirelessLastRead = 0;
let wirelessReading = false;
let wirelessPollTimer = null;
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
    const proof = await target.authorizeConsole(nonce, uid);
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

/** The only Feature 12 poll driver: read when idle and due, otherwise re-arm one timer. */
function pollWireless() {
  clearTimeout(wirelessPollTimer);
  wirelessPollTimer = null;
  const target = client;
  if (!target.state.connected || !(target.state.hardware?.capabilities & CAP.WIRELESS)) {
    wirelessLastRead = 0;
    return;
  }
  if (wirelessReading) return; // The read's finally re-polls.
  const wait = wirelessLastRead + wirelessPollInterval(target.state.wireless) - Date.now();
  if (wait <= 0 && !target.state.busy && !target.state.reboot_pending && !target.outstanding
    && !target.wirelessOperation && !wirelessForm.operationBusy()) {
    void readWirelessNow(target);
    return;
  }
  wirelessPollTimer = setTimeout(pollWireless, Math.max(wait, 250));
  wirelessPollTimer.unref?.();
}

async function readWirelessNow(target) {
  const device = target.device;
  const generation = target.generation;
  wirelessReading = true;
  wirelessLastRead = Date.now();
  try {
    await readWireless(target, device);
  } catch {
    if (target === client && device === target.device && generation === target.generation) target.expireReports();
  } finally {
    wirelessReading = false;
    pollWireless();
  }
}

/** Apply connection side effects, then render once and drive the wireless poll. */
function refresh() {
  refreshQueued = false;
  const state = client.state;
  if (!state.connected) {
    wirelessForm.reset();
    settingsDirty = false;
    for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) dialog.close();
  }
  render(state);
  if (state.connected && !state.busy && operationEligibility(state).storageReady) void wirelessForm.load();
  pollWireless();
}

/** Pure view update: reads state and form status, writes only DOM. */
function render(state) {
  const wireless = state.wireless;
  let consoleUrl = null;
  if (wireless?.wifi_enabled && wireless.wifi_state === "connected" && wireless.tcp_cli_started
    && wireless.ipv4 !== "0.0.0.0") {
    // Firmware always requires TCP CLI auth; the token pairs this tab with the console tab it opens.
    consoleAuthToken ??= randomHex();
    const query = new URLSearchParams({ ipv4: wireless.ipv4, auth_token: consoleAuthToken });
    consoleUrl = `console.html?${query}`;
  }
  const eligibility = operationEligibility(state);
  renderStatus({ ...state, console_url: consoleUrl, eligibility });

  const bleClient = client instanceof PicoForgeBluetooth;
  renderControls(state, { eligibility, bleClient, bleName: bleClient ? client.bleDevice?.name : null,
    settingsDirty, wirelessDirty: wirelessForm.dirty(), wirelessCanSave: wirelessForm.canSave() });
  wirelessForm.update(state);
  if (state.link === "ble") settingsRadios.forEach(control => { control.disabled = true; });
}

async function run(operation) {
  try { await operation(); } catch (error) { client.update({ error: error.message }); }
}

for (const [id, dialog, bluetooth] of [["usb-open", usbDialog, false],
  ["bluetooth-open", bluetoothDialog, true]]) {
  element(id).addEventListener("click", () => {
    if (element(id).disabled) return;
    if ((client instanceof PicoForgeBluetooth) !== bluetooth) {
      client.dispose();
      client = bluetooth
        ? new PicoForgeBluetooth(globalThis.isSecureContext ? navigator.bluetooth : undefined, scheduleRefresh)
        : new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, scheduleRefresh);
    }
    scheduleRefresh();
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
  dialog.addEventListener("close", () => { element("ble-control-key").value = ""; });
}
ledControl.addEventListener("click", () => run(() => client.setLed(!client.state.runtime?.led_on)));
mscControl.addEventListener("click", () => run(async () => {
  if (mscControl.disabled) return;
  const enabled = client.state.msc?.enabled;
  if (enabled) returnDialog.showModal();
  else shareDialog.showModal();
}));
element("self-test-start").addEventListener("click", () => run(() =>
  client.setSelfTest("start", element("self-test-size").valueAsNumber)));
element("self-test-cancel").addEventListener("click", () => run(() => client.setSelfTest("cancel")));
element("self-test-size").addEventListener("input", scheduleRefresh);
for (const control of [settingsClock, settingsStorage, ...settingsRadios]) {
  control.addEventListener("change", () => {
    settingsDirty = Number(settingsClock.value) !== client.state.settings?.system_clock_mhz
      || Number(settingsStorage.value) !== client.state.settings?.sdcard_profile
      || selectedRadios() !== (client.state.settings?.radios ?? 0);
    scheduleRefresh();
  });
}
settingsSave.addEventListener("click", () => {
  if (settingsSave.disabled) return;
  const bluetooth = bluetoothLink();
  configurationForceSave = Boolean(client.state.msc?.enabled);
  element("configuration-force-warning").hidden = !configurationForceSave;
  const wirelessWarning = element("configuration-wireless-warning");
  wirelessWarning.textContent = bluetooth
    ? "This saves the TCP CLI switch without sending Wi-Fi credentials or the Control key. "
      + "Bluetooth is authenticated but not encrypted."
    : "Wi-Fi and Bluetooth credentials are saved with these settings. Keep a copy of the Control key; "
      + "it cannot be read back. Flash credentials are not encrypted.";
  wirelessWarning.hidden = !wirelessForm.dirty();
  const clock = Number(settingsClock.value);
  element("configuration-dialog-profile").textContent =
    `System clock: ${clock ? `${clock} MHz` : "Default"}. `
    + `SD Card: ${Number(settingsStorage.value) === 1 ? "High-Speed" : "Standard"}.`
    + (!bluetooth && client.state.hardware?.capabilities & CAP.WIRELESS
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
  const bluetooth = bluetoothLink();
  const tcpCliEnabled = bluetooth && wirelessForm.dirty()
    ? element("settings-tcp-cli").value === "1" : undefined;
  const settings = { system_clock_mhz: Number(settingsClock.value), sdcard_profile: Number(settingsStorage.value),
    radios: bluetooth ? client.state.settings?.radios ?? 0 : selectedRadios() };
  const withCredentials = !bluetooth && wirelessForm.dirty();
  try {
    if (withCredentials) {
      wirelessForm.finish(await saveWireless(client, { ...wirelessForm.request(),
        wifi_enabled: Boolean(settings.radios & 1), ble_enabled: Boolean(settings.radios & 2), board: settings }));
    } else {
      await client.saveSettings(settings, configurationForceSave, tcpCliEnabled);
      if (tcpCliEnabled !== undefined) wirelessForm.finish({ tcp_cli_enabled: tcpCliEnabled });
    }
    settingsDirty = false;
    client.update({ settings_message: "Settings saved. Waiting for the board to reboot." });
  } catch (error) {
    if (withCredentials) wirelessForm.finish(null, error.message);
    client.update({ settings_message: `Save failed: ${error.message}` });
  }
}));
settingsReboot.addEventListener("click", () => {
  if (settingsReboot.disabled) return;
  element("reboot-consequence").textContent = client.state.wireless?.staged_mask
    || client.state.wireless?.staged_state & 8
    ? "Staged wireless credentials and their expiry marker will be discarded."
    : settingsDirty || wirelessForm.dirty()
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
}
// ponytail: handle Escape on keydown instead of relying on the native cancel event, because a repeated Escape may
// close a modal without a cancelable cancel event; connection dialogs stay open while a link is being set up.
for (const dialog of [usbDialog, bluetoothDialog, configurationDialog, rebootDialog, returnDialog, shareDialog]) {
  dialog.addEventListener("keydown", event => {
    if (event.key !== "Escape") return;
    event.preventDefault();
    if (!client.state.busy || ![usbDialog, bluetoothDialog].includes(dialog)) dialog.close();
  });
}
if (consoleAuthChannel) {
  consoleAuthChannel.onmessage = event => { void provideConsoleProof(event.data); };
  window.addEventListener?.("pagehide", () => consoleAuthChannel.close(), { once: true });
}
refresh();
