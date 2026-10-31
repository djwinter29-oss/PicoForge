import { PicoForgeWebHid } from "./device.mjs?v=4";
import { renderStatus, renderSelfTest } from "./view.mjs?v=6";
import { PicoForgeBluetooth } from "./bluetooth.mjs";
import { bindWirelessForm } from "./wireless-form.mjs?v=2";
import { decodeWireless, saveWireless } from "./wireless.mjs?v=2";

const element = id => document.querySelector(`#${id}`);
const connectionControl = element("connection-control");
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
const wirelessForm = bindWirelessForm({
  supported: () => Boolean(client.state.hardware?.capabilities & 128),
  ready: () => client.state.connected && client.state.transport !== "ble" && !client.state.busy &&
    !client.state.outcome_uncertain && !client.state.reboot_pending &&
    client.state.freshness[10] === "fresh" && client.state.msc && !client.state.msc.enabled &&
    !client.state.msc.transition_pending && !client.state.msc_command_pending &&
    !client.state.self_test_command_pending && !client.state.self_test?.active &&
    (!(client.state.hardware?.capabilities & 32) ||
      (client.state.freshness[11] === "fresh" && client.state.self_test?.valid_for_control)),
  load: async () => decodeWireless(await client.device.receiveFeatureReport(30)),
  changed: () => render(client.state),
});
let wirelessLoadedAt = 0;

function render(state) {
  renderStatus(state);
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
  const blocked = state.busy || state.outcome_uncertain || state.reboot_pending || !state.connected;
  const bluetooth = state.transport === "ble";
  const selfTestReady = !(state.hardware?.capabilities & 32)
    || (state.freshness[11] === "fresh" && state.self_test?.valid_for_control);
  const storageReady = state.freshness[10] === "fresh" && selfTestReady
    && !state.msc?.transition_pending && !state.self_test?.active
    && !state.self_test_command_pending && !state.msc_command_pending;
  connectionControl.textContent = state.connected || state.reconnecting ? "Disconnect" : "Connect";
  connectionControl.setAttribute("aria-label", state.connected || state.reconnecting
    ? "Disconnect from PicoForge board" : "Connect to PicoForge board");
  connectionControl.disabled = state.busy || !globalThis.isSecureContext ||
    (element("connection-transport").value === "ble" ? !navigator.bluetooth : !navigator.hid);
  element("connection-transport").disabled = state.connected || state.busy;
  ledControl.disabled = blocked || !state.board_status?.led_available || !(state.hardware?.capabilities & 1);
  mscControl.disabled = blocked || !storageReady || bluetooth;
  mscControl.textContent = state.msc?.enabled ? "Return to Pico" : "Enable USB drive";
  const settingsReady = !blocked && storageReady && !bluetooth;
  for (const id of ["settings-wifi", "settings-ble"]) {
    document.querySelector(`label[for="${id}"]`).hidden = !(state.hardware?.capabilities & 128);
  }
  for (const control of [settingsClock, settingsStorage, ...settingsRadios, settingsSave]) {
    control.disabled = !settingsReady || !state.settings_supported;
  }
  settingsSave.disabled ||= !settingsDirty && !wirelessForm.dirty();
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
  wirelessForm.update();
  if (Date.now() - wirelessLoadedAt >= 2000 && state.connected && storageReady && !state.busy && !bluetooth &&
    (state.hardware?.capabilities & 128)) {
    wirelessLoadedAt = Date.now();
    void wirelessForm.load();
  }
}

async function run(operation) {
  try { await operation(); } catch (error) { client.update({ error: error.message }); }
}

connectionControl.addEventListener("click", () => run(() =>
  client.state.connected || client.state.reconnecting ? client.disconnect() : client.connect()));
element("connection-transport").addEventListener("change", () => {
  if (client.state.connected || client.state.busy || client.outstanding) return;
  client = element("connection-transport").value === "ble"
    ? new PicoForgeBluetooth(navigator.bluetooth, render)
    : new PicoForgeWebHid(navigator.hid, render);
  render(client.state);
});
element("ble-authorize").addEventListener("click", () => run(async () => {
  if (!(client instanceof PicoForgeBluetooth)) throw new Error("Select Bluetooth LE first.");
  try { await client.setControlKey(element("ble-control-key").value); }
  finally { element("ble-control-key").value = ""; }
}));
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
element("self-test-size").addEventListener("input", () => renderSelfTest(client.state));
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
      wirelessForm.finish(await saveWireless(client, { ...wirelessForm.request(), board: settings }));
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
  element("reboot-consequence").textContent = settingsDirty
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
