// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
import { acknowledge, reconciledResult, responseJson, savedSettings, scannedNetworks } from "../../src/js/dashboard-api.mjs";
import { commandEffects, commandSettled, operationState, unresolvedCommand } from "../../src/js/operation-state.mjs";

const state = operationState({ connected: true }, { uncertain: false }, 32);
state.canStartSelfTest.valueOf();
// @ts-expect-error Admission state must use booleans, not truthy strings.
operationState({}, { rebootPending: "false" });
// @ts-expect-error Benchmark size is numeric.
operationState({}, {}, "32");
const result = await responseJson(new Response("{}"), "save", true);
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
