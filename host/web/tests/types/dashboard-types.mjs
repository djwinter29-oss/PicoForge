// @ts-check
import { acknowledge, responseJson, savedSettings } from "../../src/js/dashboard-api.mjs";
import { operationState } from "../../src/js/operation-state.mjs";

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
