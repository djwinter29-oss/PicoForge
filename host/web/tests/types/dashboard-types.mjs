// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
import {
  acknowledge, reconciledResult, responseJson, savedSettings, scannedNetworks, statusResponse, testedWireless,
} from "../../src/js/dashboard-api.mjs";
import {
  commandEffects, commandSettled, createCommandState, observeCommandStatus, operationState, unresolvedCommand,
} from "../../src/js/operation-state.mjs";
import { bindWirelessForm } from "../../src/js/wireless-form.mjs";
import { initializeSettings, normalizeStatus, renderStatus } from "../../src/js/dashboard.mjs";

const state = operationState({ connected: true }, { uncertain: false }, 32);
state.canStartSelfTest.valueOf();
state.canSaveSettings.valueOf();
state.saveBlockReason?.toUpperCase();
// @ts-expect-error Save draft ownership is boolean, not a string.
operationState({}, { settingsDirty: "true" });
// @ts-expect-error Block reasons are text, not booleans.
operationState({}, { wirelessBlockReason: false });
// @ts-expect-error View eligibility must use booleans.
renderStatus({}, { ...state, canSaveSettings: "true" });
// @ts-expect-error Settings drafts must use numeric clock values.
initializeSettings({ settings_draft: { system_clock_mhz: "250", sdcard_profile: 0, radios: 1 } });
// @ts-expect-error Normalization cannot accept arbitrary JSON fields without validation.
normalizeStatus({ board_configuration: {} });
// @ts-expect-error Radio draft ownership is boolean, not a radio bitmask.
operationState({}, { radiosDirty: 3 });
const wirelessForm = bindWirelessForm({
  scan: async () => [], cancelTest: async () => {}, ready: () => true, supported: () => true,
  test: async () => ({ remaining_seconds: 600, test_generation: 1, test_id: "a".repeat(32) }),
});
wirelessForm.radioEdited("wifi");
// @ts-expect-error Radio draft serialization requires numeric saved bits.
wirelessForm.selectedRadios("3");
const credentials = wirelessForm.credentialActions();
/** @type {import("../../src/js/wireless-form.mjs").CredentialAction} */
const preserve = credentials.wifi;
// @ts-expect-error A credential action cannot be an arbitrary command.
credentials.wifi = "delete";
// @ts-expect-error Radio intent cannot refer to an unrelated setting.
wirelessForm.radioEdited("clock");
// @ts-expect-error Form baselines require SSID information.
wirelessForm.initialize({ wifi_enabled: true });
const commands = createCommandState();
/** @type {import("../../src/js/operation-state.mjs").UncertainCommand} */
const lostBenchmark = { kind: "self-test", message: "uncertain" };
commands.uncertainCommand = lostBenchmark;
commands.selfTestPendingSequence = "awaiting-status";
// @ts-expect-error Observation fences cannot use arbitrary states.
commands.selfTestPendingSequence = "completed";
observeCommandStatus(commands, { self_test: { sequence: 7 }, self_test_stale: false }, false);
// @ts-expect-error A periodic report counter is not an uncertain command's identity.
commands.uncertainCommand = { kind: "self-test", sequence: 7, message: "uncertain" };
// @ts-expect-error Reboot transitions cannot be arbitrary strings.
commands.rebootPending = "finished";
// @ts-expect-error Ownership targets are booleans, not strings.
commands.mscPendingEnabled = "false";
// @ts-expect-error Transition freshness flags are boolean.
observeCommandStatus(commands, { self_test_stale: "false" }, false);
// @ts-expect-error Admission state must use booleans, not truthy strings.
operationState({}, { rebootPending: "false" });
// @ts-expect-error Benchmark size is numeric.
operationState({}, {}, "32");
const result = await responseJson(new Response("{}"), "save", true);
const normalized = normalizeStatus(statusResponse(result));
normalized.memory?.manufacturer?.serial_number?.toFixed();
// @ts-expect-error Manufacturer diagnostics may be partial even when the manufacturer object is present.
normalized.memory?.manufacturer?.serial_number.toFixed();
// @ts-expect-error Validated manufacturer serial numbers are numeric, never strings.
normalized.memory?.manufacturer?.serial_number?.toUpperCase();
normalized.settings?.system_clock_mhz.toFixed();
// @ts-expect-error Normalized settings preserve numeric types.
normalized.settings?.radios.toUpperCase();
// @ts-expect-error Non-object replies cannot reach status validation.
statusResponse(null);
const tested = testedWireless(result.test);
tested.test_generation.toFixed();
tested.test_id.toLowerCase();
// @ts-expect-error Validated Wi-Fi test identities are opaque strings, not counters.
tested.test_id.toFixed();
// @ts-expect-error Validated Wi-Fi generations are numeric, not strings.
tested.test_generation.toUpperCase();
// @ts-expect-error The API's test reply must be validated before using its identity.
result.test.test_generation;
// @ts-expect-error Unvalidated JSON fields are unknown.
result.settings.system_clock_mhz;
const saved = savedSettings(result, { system_clock_mhz: 200, sdcard_profile: 0, radios: 0 });
saved.radios.toFixed();
// @ts-expect-error Boot settings fields must be numeric.
savedSettings(result, { system_clock_mhz: 200, sdcard_profile: "0", radios: 0 });
acknowledge(result, 1);
scannedNetworks(result)[0].ssid_hex.toLowerCase();
// @ts-expect-error Validated signal strength is numeric.
scannedNetworks(result)[0].rssi.toUpperCase();
commandSettled({ kind: "led", on: true, message: "uncertain" }, {});
commandSettled({ kind: "reconnect-status", opcode: 3, disconnected: false, message: "uncertain" }, {});
const unresolved = unresolvedCommand(null, true, false, null);
if (unresolved?.kind === "reconnect-status") unresolved.disconnected.valueOf();
// @ts-expect-error Reconnect recovery must name a storage or benchmark command.
commandSettled({ kind: "reconnect-status", opcode: 1, disconnected: false, message: "uncertain" }, {});
// @ts-expect-error Reconnect observation is a boolean, not a truthy string.
commandSettled({ kind: "reconnect-status", opcode: 4, disconnected: "true", message: "uncertain" }, {});
// @ts-expect-error Storage recovery carries the requested boolean ownership state.
commandSettled({ kind: "msc", enabled: "true", message: "uncertain" }, {});
// @ts-expect-error LED recovery must carry the requested boolean state.
commandSettled({ kind: "led", on: "true", message: "uncertain" }, {});
// @ts-expect-error Recovery reasons cannot be arbitrary strings.
commandSettled({ kind: "retry", message: "uncertain" }, {});
const effects = commandEffects(acknowledge(result, 4));
effects.releaseSelfTest.valueOf();
const reconciled = reconciledResult(result);
if (reconciled) commandEffects(reconciled);
// @ts-expect-error Raw response fields must be validated before result publication.
commandEffects(result.control_result);
// @ts-expect-error Pending results cannot release lifecycle obligations.
commandEffects({ opcode: 4, status: "pending" });
// @ts-expect-error Result opcodes must be numeric.
commandEffects({ opcode: "4", status: "failed" });
