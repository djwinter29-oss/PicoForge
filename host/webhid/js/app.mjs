import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus, renderSelfTest, storageSpeed } from "./view.mjs?v=3";

const connectionControl = document.querySelector("#connection-control");
const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const settingsSave = document.querySelector("#settings-save");
const settingsClock = document.querySelector("#settings-clock");
const settingsStorage = document.querySelector("#settings-storage");
let settingsDirty = false;
let settingsStorageDirty = false;
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  if (!state.connected) {
    settingsDirty = false;
    settingsStorageDirty = false;
  }
  if (!settingsDirty && state.settings) {
    settingsClock.value = String(state.settings.system_clock_mhz);
  }
  if (!settingsStorageDirty && state.settings) {
    settingsStorage.value = String(storageSpeed(state.settings, state.memory?.mode));
  }
  connectionControl.textContent = state.connected ? "Disconnect" : "Connect";
  connectionControl.setAttribute("aria-label", state.connected
    ? "Disconnect from PicoForge board" : "Connect to PicoForge board");
  connectionControl.disabled = state.busy || state.reconnecting || !navigator.hid;
  ledControl.disabled = state.busy || !state.connected || !state.board_status?.led_available;
  mscControl.disabled = state.busy || !state.connected || !state.msc || Boolean(state.msc.transition_pending)
    || Boolean(state.self_test?.active) || state.self_test_command_pending
    || Boolean(state.msc?.enabled && !state.msc.force_return_supported);
  mscControl.textContent = state.msc?.enabled ? "Return to Pico" : "Enable USB drive";
  const settingsReady = state.connected && state.settings_supported && state.settings !== null && Boolean(state.msc)
    && !state.msc.enabled && !state.msc.transition_pending && !state.self_test?.active
    && !state.self_test_command_pending;
  for (const control of [settingsClock, settingsStorage]) {
    control.disabled = state.busy || !settingsReady;
  }
  settingsSave.disabled = state.busy || !settingsReady;
  document.querySelector("#settings-note").textContent = state.settings_message
    || (!state.connected ? "Connect to read the saved settings."
      : !state.settings_supported ? "Settings are unavailable; update the device firmware."
        : !state.msc ? "Waiting for storage ownership status."
          : state.msc.enabled || state.msc.transition_pending ? "Disable the USB drive before saving settings."
            : "Settings loaded.");
  document.querySelector("#msc-note").textContent = state.reconnecting
    ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.enabled && !state.msc.force_return_supported
    ? "Update firmware to disable the USB drive from Pico; this firmware requires manual host eject."
    : state.msc?.request_rejected
    ? (state.msc.enabled ? "Request refused. Disabling requires idle storage and successful device sync. Check diagnostics."
      : "Storage return failed. Volume may be unmounted; inspect media and UART errors before mounting.")
    : state.msc?.transition_pending ? "Preparing storage and changing USB mode; HID reconnects automatically."
      : "";
  document.querySelector("#msc-note").hidden = !document.querySelector("#msc-note").textContent;
}

connectionControl.addEventListener("click", () => {
  if (client.state.connected) void client.disconnect();
  else void client.connect();
});
ledControl.addEventListener("click", () => client.setLed(!client.state.board_status?.led_on));
mscControl.addEventListener("click", () => {
  const enabled = client.state.msc?.enabled;
  if (enabled && (!client.state.msc.force_return_supported
    || !window.confirm("Disable USB drive and return storage to Pico? "
      + "Cached host writes may be lost and the filesystem may be corrupted. "
      + "Stop all host file activity first. Pico will attempt to remount without formatting or repair. Continue?"))) return;
  void client.setMsc(!enabled, Boolean(enabled)).catch(error => client.update({ error: error.message }));
});
document.querySelector("#self-test-start").addEventListener("click", async () => {
  await client.setSelfTest("start", document.querySelector("#self-test-size").valueAsNumber);
  render(client.state);
});
document.querySelector("#self-test-cancel").addEventListener("click", () => client.setSelfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(client.state));
function selectedSettings() {
  const saved = client.state.settings;
  const storageMhz = Number(settingsStorage.value);
  return {
    system_clock_mhz: Number(settingsClock.value),
    sdio_bus_mhz: settingsStorageDirty ? storageMhz : (saved?.sdio_bus_mhz ?? 0),
    spi_bus_mhz: settingsStorageDirty ? storageMhz : (saved?.spi_bus_mhz ?? 0),
  };
}

function updateSettingsDirty() {
  const saved = client.state.settings;
  if (!saved) return;
  settingsStorageDirty = Number(settingsStorage.value) !== storageSpeed(saved, client.state.memory?.mode);
  settingsDirty = Number(settingsClock.value) !== saved.system_clock_mhz || settingsStorageDirty;
  render(client.state);
}

for (const control of [settingsClock, settingsStorage]) {
  control.addEventListener("change", () => {
    updateSettingsDirty();
  });
}
settingsSave.addEventListener("click", async () => {
  const saved = await client.saveSettings(selectedSettings());
  if (saved) {
    settingsDirty = false;
    settingsStorageDirty = false;
  }
  render(client.state);
});
render(client.state);