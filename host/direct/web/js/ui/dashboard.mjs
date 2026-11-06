import { CAP, STAGED_EXPIRED } from "../protocol/constants.mjs";
import { createDashboardView, renderControls } from "./dashboard-view.mjs";
import { bindWirelessForm } from "./sections/wireless-form.mjs";
import { bindConsoleLink } from "./sections/console-link.mjs";
import { canRun } from "../state/selectors.mjs";
import { createUiStore, UI_INITIAL } from "../state/store.mjs";
import { parseHexBytes } from "../protocol/codec.mjs";
import { queryElement } from "./dom.mjs";
import { deriveDashboardViewModel } from "./sections/status-model.mjs";
import { describe } from "./messages.mjs";

/**
 * @brief Wire the dashboard DOM to a board session: subscribe to its domain store, send user intents to it, render.
 * @param environment Injected browser facts: `{secure, hid, ble, now, clipboard}`; the UI never reads globals.
 */
export function mountDashboard({ session, selectTransport, timers, environment }) {
  const element = id => queryElement(`#${id}`);
  let client = session;
  let disposed = false;
  const ui = createUiStore();
  const setUi = changes => ui.dispatch({ type: "patch", changes });
  const renderStatus = createDashboardView();
  const consoleLink = bindConsoleLink(() => client, error => setUi({ action_error: describe(error) }));
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
    now: environment.now,
    clipboard: environment.clipboard,
    ready: () => canRun(client.state, "wireless").allowed,
    tcpReady: () => bluetoothLink() ? canRun(client.state, "settings").allowed
      : canRun(client.state, "wireless").allowed && Number(settingsRadios[0].value) === 1,
    scan: signal => client.scanWireless(signal),
    test: (settings, onProgress, signal) => client.testWireless(settings, onProgress, signal),
    cancelTest: signal => client.cancelWireless(signal),
    changed: () => setUi({ wireless_form_busy: wirelessForm.operationBusy() }),
  });
  ui.subscribe(scheduleRefresh);
  let watched = null;
  let unwatch = () => {};
  /** Follow the session's domain store; a transport switch may hand over a session with a different store. */
  const watch = () => {
    if (watched === client.store) return;
    unwatch();
    watched = client.store;
    unwatch = watched.subscribe(scheduleRefresh);
  };
  watch();
  /** Reset dashboard-only state after a disconnect, then render once. Performs no device I/O. */
  function refresh() {
    refreshQueued = false;
    if (disposed) return;
    const board = client.state;
    if (!board.connected) {
      wirelessForm.reset();
      if (Object.entries(UI_INITIAL).some(([key, value]) => ui.getState()[key] !== value)) {
        ui.dispatch({ type: "replace", state: { ...UI_INITIAL } });
      }
      for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) dialog.close();
    }
    render(board, ui.getState());
  }

  /** View update: reads board and UI state, writes only DOM and the form's adopted snapshot. */
  function render(board, view) {
    // Domain state carries reason codes; this is the one place they become text.
    const state = { ...board, error: describe(board.error) ?? view.action_error,
      recovery_message: describe(board.recovery_message), wireless_error: describe(board.wireless_error),
      wireless_poll_error: describe(board.wireless_poll_error) };
    const viewModel = deriveDashboardViewModel(state, view.wireless_form_busy);
    renderStatus({ ...state, ...viewModel, open_console: consoleLink.open });
    renderControls(state, { eligibility: viewModel.eligibility, formBusy: view.wireless_form_busy,
      settingsMessage: view.settings_message, bleName: client.transport.name, settingsDirty: view.settings_dirty,
      wirelessDirty: wirelessForm.dirty(), wirelessCanSave: wirelessForm.canSave() }, environment);
    wirelessForm.update(board);
    if (board.link === "ble") settingsRadios.forEach(control => { control.disabled = true; });
  }

  async function run(operation) {
    setUi({ action_error: null });
    try { await operation(); } catch (error) { setUi({ action_error: describe(error) }); }
  }

  for (const [id, dialog, bluetooth] of [["usb-open", usbDialog, false],
    ["bluetooth-open", bluetoothDialog, true]]) {
    element(id).addEventListener("click", () => {
      if (element(id).disabled) return;
      if ((client.state.link === "ble") !== bluetooth) {
        client = selectTransport(bluetooth ? "ble" : "usb");
        watch();
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
    const key = element("direct-ble-control-key").value;
    element("direct-ble-control-key").value = "";
    await client.connect(parseHexBytes(key, 32, "control-key"));
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
      setUi({ settings_dirty: Number(settingsClock.value) !== client.state.settings?.system_clock_mhz
        || Number(settingsStorage.value) !== client.state.settings?.sdcard_profile
        || selectedRadios() !== (client.state.settings?.radios ?? 0) });
      scheduleRefresh();
    });
  }
  settingsSave.addEventListener("click", () => {
    if (settingsSave.disabled) return;
    const bluetooth = bluetoothLink();
    setUi({ configuration_force_save: Boolean(client.state.msc?.enabled) });
    element("configuration-force-warning").hidden = !ui.getState().configuration_force_save;
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
    const force = ui.getState().configuration_force_save;
    if (Boolean(client.state.msc?.enabled) !== force) return;
    const bluetooth = bluetoothLink();
    const tcpCliEnabled = bluetooth && wirelessForm.dirty()
      ? element("direct-settings-tcp-cli").value === "1" : undefined;
    const settings = { system_clock_mhz: Number(settingsClock.value), sdcard_profile: Number(settingsStorage.value),
      radios: bluetooth ? client.state.settings?.radios ?? 0 : selectedRadios() };
    let wireless = null;
    try {
      wireless = wirelessForm.request();
      const saved = await client.saveConfiguration({ settings, force, tcpCliEnabled,
        wireless });
      if (saved) wirelessForm.finish(saved);
      setUi({ settings_dirty: false, settings_message: "Settings saved. Waiting for the board to reboot." });
    } catch (error) {
      if (!bluetooth && wirelessForm.dirty()) wirelessForm.finish(null, describe(error));
      setUi({ settings_message: `Save failed: ${describe(error)}` });
    }
  }));
  settingsReboot.addEventListener("click", () => {
    if (settingsReboot.disabled) return;
    element("reboot-consequence").textContent = client.state.wireless?.staged_mask
      || client.state.wireless?.staged_state & STAGED_EXPIRED
      ? "Staged wireless credentials and their expiry marker will be discarded."
      : ui.getState().settings_dirty || wirelessForm.dirty()
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
      unwatch();
      wirelessForm.dispose();
      consoleLink.dispose();
    },
  };
}
