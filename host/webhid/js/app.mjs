import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus, renderSelfTest } from "./view.mjs";

const element = id => document.querySelector(`#${id}`);
const connectionControl = element("connection-control");
const ledControl = element("led-control");
const mscControl = element("msc-control");
const settingsSave = element("settings-save");
const settingsReboot = element("settings-reboot");
const settingsClock = element("settings-clock");
const settingsStorage = element("settings-storage");
let settingsDirty = false;
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  if (!state.connected) settingsDirty = false;
  if (!settingsDirty && state.settings) {
    settingsClock.value = String(state.settings.system_clock_mhz);
    settingsStorage.value = String(state.settings.sdcard_profile);
  }
  const blocked = state.busy || state.outcome_uncertain || state.reboot_pending || !state.connected;
  const selfTestReady = !(state.hardware?.capabilities & 32)
    || (state.freshness[11] === "fresh" && state.self_test?.valid_for_control);
  const storageReady = state.freshness[10] === "fresh" && selfTestReady
    && !state.msc?.transition_pending && !state.self_test?.active
    && !state.self_test_command_pending && !state.msc_command_pending;
  connectionControl.textContent = state.connected || state.reconnecting ? "Disconnect" : "Connect";
  connectionControl.setAttribute("aria-label", state.connected || state.reconnecting
    ? "Disconnect from PicoForge board" : "Connect to PicoForge board");
  connectionControl.disabled = state.busy || !globalThis.isSecureContext || !navigator.hid;
  ledControl.disabled = blocked || !state.board_status?.led_available || !(state.hardware?.capabilities & 1);
  mscControl.disabled = blocked || !storageReady;
  mscControl.textContent = state.msc?.enabled ? "Return to Pico" : "Enable USB drive";
  const settingsReady = !blocked && storageReady && !state.msc?.enabled;
  for (const control of [settingsClock, settingsStorage, settingsSave]) {
    control.disabled = !settingsReady || !state.settings_supported;
  }
  settingsReboot.disabled = !settingsReady || !(state.hardware?.capabilities & 2);
  element("settings-note").textContent = state.settings_message
    || (!state.connected ? "Connect to read the saved settings."
      : !state.settings_supported ? "Persistent settings save is unavailable."
        : !storageReady ? "Waiting for fresh, idle storage and self-test status."
          : state.msc?.enabled ? "Return storage to Pico before saving settings."
            : "Stored profiles apply at next boot; saving does not reboot.");
  element("msc-note").textContent = state.reconnecting
    ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.request_rejected ? "Storage request or transition failed; inspect storage and CDC diagnostics."
      : state.msc?.transition_pending || state.msc_command_pending
        ? "Waiting for the storage ownership transition result."
        : state.msc?.enabled ? "Host owns storage. Safely eject before return whenever possible." : "";
  element("msc-note").hidden = !element("msc-note").textContent;
}

async function run(operation) {
  try { await operation(); } catch (error) { client.update({ error: error.message }); }
}

connectionControl.addEventListener("click", () => run(() =>
  client.state.connected || client.state.reconnecting ? client.disconnect() : client.connect()));
ledControl.addEventListener("click", () => run(() => client.setLed(!client.state.board_status?.led_on)));
mscControl.addEventListener("click", () => run(async () => {
  const enabled = client.state.msc?.enabled;
  if (enabled && !window.confirm("Force return storage to Pico without a safe-eject guarantee? "
    + "Cached host writes may be lost and the filesystem may be corrupted. "
    + "Stop all host file activity and safely eject whenever possible. "
    + "Pico attempts a remount without formatting or repair. Continue?")) return;
  await client.setMsc(!enabled, Boolean(enabled));
}));
element("self-test-start").addEventListener("click", () => run(() =>
  client.setSelfTest("start", element("self-test-size").valueAsNumber)));
element("self-test-cancel").addEventListener("click", () => run(() => client.setSelfTest("cancel")));
element("self-test-size").addEventListener("input", () => renderSelfTest(client.state));
for (const control of [settingsClock, settingsStorage]) {
  control.addEventListener("change", () => {
    settingsDirty = Number(settingsClock.value) !== client.state.settings?.system_clock_mhz
      || Number(settingsStorage.value) !== client.state.settings?.sdcard_profile;
    render(client.state);
  });
}
settingsSave.addEventListener("click", () => run(async () => {
  if (await client.saveSettings({ system_clock_mhz: Number(settingsClock.value),
    sdcard_profile: Number(settingsStorage.value) })) settingsDirty = false;
  render(client.state);
}));
settingsReboot.addEventListener("click", () => run(async () => {
  if (window.confirm("Reboot PicoForge to apply the stored next-boot profiles?")) await client.reboot(true);
}));
render(client.state);
