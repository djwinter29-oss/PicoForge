import { createDashboardStore, createUiStore } from "../../web/js/state/store.mjs";
import { bindWirelessForm } from "../../web/js/ui/sections/wireless-form.mjs";
import { mountDashboard } from "../../web/js/ui/dashboard.mjs";
import { deriveControlsViewModel } from "../../web/js/ui/sections/controls.mjs";
import { queryElement } from "../../web/js/ui/dom.mjs";

const keyInput = queryElement("input#direct-ble-secret");
keyInput.value = "";
// @ts-expect-error Input values are strings.
keyInput.value = 32;
// @ts-expect-error Untagged lookups do not promise input or dialog methods.
queryElement("#unknown").showModal();
// @ts-expect-error A select is not a dialog.
queryElement("select#direct-settings-ble").showModal();
queryElement("dialog#direct-wifi-dialog").showModal();

const board = createDashboardStore();
board.dispatch({ type: "command/started", changes: { busy: true, pending_request: { transaction_id: 1, opcode: 1 } } });
// @ts-expect-error Busy is a boolean, not display text.
board.dispatch({ type: "command/started", changes: { busy: "yes" } });
// @ts-expect-error Transaction IDs are numbers.
board.dispatch({ type: "command/started", changes: { pending_request: { transaction_id: "1", opcode: 1 } } });
// @ts-expect-error Freshness uses the declared states.
board.dispatch({ type: "report/received", changes: { freshness: { 3: "recent" } } });
// @ts-expect-error Presentation state does not belong in domain state.
board.dispatch({ type: "connection/changed", changes: { settings_dirty: true } });

const ui = createUiStore();
ui.dispatch({ type: "patch", changes: { settings_dirty: true } });
// @ts-expect-error UI flags are booleans.
ui.dispatch({ type: "patch", changes: { settings_dirty: 1 } });
// @ts-expect-error Domain state does not belong in the UI store.
ui.dispatch({ type: "patch", changes: { connected: true } });

/** @type {Parameters<typeof bindWirelessForm>[0]} */
const formOptions = {
  ready: () => true, supported: () => true, scan: async () => [],
  test: async () => ({ remaining_seconds: 600 }), cancelTest: async () => {},
};
bindWirelessForm(formOptions);
// @ts-expect-error Form readiness must be a boolean.
bindWirelessForm({ ...formOptions, ready: () => "ready" });
// @ts-expect-error Test lifetime must be numeric.
bindWirelessForm({ ...formOptions, test: async () => ({ remaining_seconds: "600" }) });
// @ts-expect-error Cancellation must finish without a result value.
bindWirelessForm({ ...formOptions, cancelTest: async () => true });

/** @type {import("../../web/js/ui/dashboard.mjs").DashboardEnvironment} */
const environment = { secure: true, hid: true, ble: false, now: () => 0, clipboard: null };
// @ts-expect-error Browser API availability is boolean, not an API object.
environment.ble = {};
/** @param {Parameters<typeof mountDashboard>[0]} options */
function checkDashboardOptions(options) {
  // @ts-expect-error Transport selection returns a session, not a transport name.
  options.selectTransport = () => "ble";
}

/** @type {import("../../web/js/board/commands.mjs").CommandOptions} */
const command = { lifecycle: { operation: 9, generation: 1 } };
// @ts-expect-error Lifecycle generations are numeric.
command.lifecycle.generation = "1";
/** @type {import("../../web/js/board/operations.mjs").ConfigurationIntent} */
const configuration = {
  settings: { system_clock_mhz: 200, sdcard_profile: 1, radios: 0 }, force: false, wireless: null,
};
// @ts-expect-error Configuration profile is numeric.
configuration.settings.sdcard_profile = "fast";

/** @param {Parameters<typeof deriveControlsViewModel>[1]} controls */
function checkControlTypes(controls) {
  // @ts-expect-error Render readiness must be a boolean.
  controls.eligibility.settingsReady = "ready";
  // @ts-expect-error Dirty state is boolean, not display text.
  controls.settingsDirty = "changed";
  // @ts-expect-error Save blockers are user-facing text or null, not a second boolean gate.
  controls.wirelessSaveBlockReason = false;
}
/** @param {ReturnType<typeof import("../../web/js/ui/sections/wireless-form.mjs").bindWirelessForm>} form */
function checkFormSnapshot(form) {
  // @ts-expect-error Render-only readiness has boolean gates.
  form.update({}, { wireless: true, settings: "ready", tcp: true });
  /** @type {AbortSignal} */
  const signal = form.signal;
  // @ts-expect-error Save requests have a structured wireless shape, not presentation text.
  form.request = () => "saved";
  // @ts-expect-error Saved wireless flags are boolean.
  form.finish({ wifi_enabled: "yes" });
  // @ts-expect-error Draft lifecycle is an AbortSignal, not an AbortController.
  form.signal = new AbortController();
}
