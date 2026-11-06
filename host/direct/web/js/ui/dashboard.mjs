import { CAP } from "../protocol/constants.mjs";
import { createDashboardView, renderControls } from "./dashboard-view.mjs";
import { bindWirelessForm } from "./sections/wireless-form.mjs";
import { bindConsoleLink } from "./sections/console-link.mjs";
import { deriveOperationEligibility } from "../state/selectors.mjs";
import {
  cancelWireless, readWireless, saveWireless, scanWireless, testWireless,
} from "../board/workflows/wireless.mjs";
import { queryElement } from "./dom.mjs";
import { deriveDashboardViewModel } from "./sections/status-model.mjs";

export function mountDashboard({ session, selectTransport, timers }) {
  const element = id => queryElement(`#${id}`);
  let client = session;
  let disposed = false;
  const renderStatus = createDashboardView();
  const consoleLink = bindConsoleLink(() => client);
  const usbDialog = element("usb-connection-dialog");
  const bluetoothDialog = element("bluetooth-connection-dialog");
  const ledControl = element("led-control");
  const mscControl = element("msc-control");
  const settingsSave = element("settings-save");
  const settingsReboot = element("settings-reboot");
  const settingsClock = element("settings-clock");
  const settingsStorage = element("settings-storage");
  const settingsRadios = [element("direct-settings-wifi"), element("direct-settings-ble")];
  const selectedRadios = () => client.state.hardware?.capabilities & CAP.WIRELESS
    ? Number(settingsRadios[0].value) | Number(settingsRadios[1].value) << 1 : 0;
  const configurationDialog = element("configuration-dialog");
  const rebootDialog = element("reboot-dialog");
  const returnDialog = element("return-dialog");
  const shareDialog = element("share-dialog");
  let refreshQueued = false;
  /** Coalesce bursts of client state changes into one render and side-effect pass per microtask. */
  const scheduleRefresh = () => {
    if (refreshQueued || disposed) return;
    refreshQueued = true;
    queueMicrotask(refresh);
  };
  const bluetoothLink = () => client.state.link === "ble";
  let wirelessForm;
  wirelessForm = bindWirelessForm({
    supported: () => Boolean(client.state.hardware?.capabilities & CAP.WIRELESS),
    timerScope: timers,
    ready: () => client.eligibility().wirelessReady,
    loadReady: () => client.eligibility()[bluetoothLink() ? "settingsReady" : "wirelessReady"],
    tcpReady: () => bluetoothLink() ? client.eligibility().settingsReady
      : client.eligibility().wirelessReady && Number(settingsRadios[0].value) === 1,
    load: async signal => client.state.wireless ?? readWireless(client, client.device, signal),
    scan: signal => scanWireless(client, signal),
    test: (settings, onProgress, signal) => testWireless(client, settings, onProgress, signal),
    cancelTest: signal => cancelWireless(client, signal),
    changed: () => {
      client.update({ wireless_form_busy: wirelessForm.operationBusy() });
      scheduleRefresh();
    },
  });
  /** Apply connection side effects, then render once and drive the wireless poll. */
  function refresh() {
    refreshQueued = false;
    if (disposed) return;
    const state = client.state;
    if (!state.connected) {
      wirelessForm.reset();
      if (state.settings_dirty) client.update({ settings_dirty: false });
      for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) dialog.close();
    }
    render(state);
    if (state.connected && !state.busy && deriveOperationEligibility(state).storageReady) void wirelessForm.load();
    client.pollWireless();
  }

  /** Pure view update: reads state and form status, writes only DOM. */
  function render(state) {
    const viewModel = deriveDashboardViewModel(state);
    renderStatus({ ...state, ...viewModel, open_console: consoleLink.open });

    renderControls(state, { eligibility: viewModel.eligibility,
      bleName: client.transport.name,
      settingsDirty: state.settings_dirty, wirelessDirty: wirelessForm.dirty(), wirelessCanSave: wirelessForm.canSave() });
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
      if ((client.state.link === "ble") !== bluetooth) client = selectTransport(bluetooth ? "ble" : "usb");
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
    const key = element("direct-ble-control-key").value;
    element("direct-ble-control-key").value = "";
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
    dialog.addEventListener("close", () => { element("direct-ble-control-key").value = ""; });
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
      client.update({ settings_dirty: Number(settingsClock.value) !== client.state.settings?.system_clock_mhz
        || Number(settingsStorage.value) !== client.state.settings?.sdcard_profile
        || selectedRadios() !== (client.state.settings?.radios ?? 0) });
      scheduleRefresh();
    });
  }
  settingsSave.addEventListener("click", () => {
    if (settingsSave.disabled) return;
    const bluetooth = bluetoothLink();
    client.update({ configuration_force_save: Boolean(client.state.msc?.enabled) });
    element("configuration-force-warning").hidden = !client.state.configuration_force_save;
    const wirelessWarning = element("direct-configuration-wireless-warning");
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
        ? ` TCP CLI: ${element("direct-settings-tcp-cli").value === "1" ? "Enabled" : "Disabled"}.` : "");
    configurationDialog.showModal();
  });
  element("confirm-configuration").addEventListener("click", () => run(async () => {
    if (!configurationDialog.open) return;
    configurationDialog.close();
    if (settingsSave.disabled) return;
    if (Boolean(client.state.msc?.enabled) !== client.state.configuration_force_save) return;
    const bluetooth = bluetoothLink();
    const tcpCliEnabled = bluetooth && wirelessForm.dirty()
      ? element("direct-settings-tcp-cli").value === "1" : undefined;
    const settings = { system_clock_mhz: Number(settingsClock.value), sdcard_profile: Number(settingsStorage.value),
      radios: bluetooth ? client.state.settings?.radios ?? 0 : selectedRadios() };
    const withCredentials = !bluetooth && wirelessForm.dirty();
    try {
      if (withCredentials) {
        wirelessForm.finish(await saveWireless(client, { ...wirelessForm.request(),
          wifi_enabled: Boolean(settings.radios & 1), ble_enabled: Boolean(settings.radios & 2), board: settings }));
      } else {
        await client.saveSettings(settings, client.state.configuration_force_save, tcpCliEnabled);
        if (tcpCliEnabled !== undefined) wirelessForm.finish({ tcp_cli_enabled: tcpCliEnabled });
      }
      client.update({ settings_dirty: false });
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
      : client.state.settings_dirty || wirelessForm.dirty()
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
  refresh();
  return {
    refresh: scheduleRefresh,
    dispose() {
      disposed = true;
      wirelessForm.dispose();
      consoleLink.dispose();
    },
  };
}
