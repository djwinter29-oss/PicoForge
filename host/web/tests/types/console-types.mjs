// @ts-check
import { consoleRequest, validateConsoleResponse } from "../../src/js/console-api.mjs";
import { ConsoleTerminal } from "../../src/js/console-terminal.mjs";

/** @type {import("../../src/js/app.mjs").OperationSnapshot} */
const snapshot = { connected: true, msc: { transition_pending: false }, self_test: { active: true } };
// @ts-expect-error Command-state fields must not use truthy strings.
snapshot.command_outcome_uncertain = "false";
/** @type {import("../../src/js/app.mjs").OperationState} */
const operations = {
  outcomeUncertain: false, mscPending: false, selfTestPending: false, storageCommandPending: false,
  busy: false, settingsBusy: false, canSetLed: true, canChangeMsc: true, boardSelectionBusy: false,
};
// @ts-expect-error Eligibility must not use numeric sentinels.
operations.canChangeMsc = 1;
const terminal = new ConsoleTerminal();
// @ts-expect-error Terminal input must be decoded text.
terminal.write(new Uint8Array());

const read = await consoleRequest("/api/console");
read.data.toUpperCase();
read.closed.valueOf();
// @ts-expect-error A read response is not a session creation response.
read.session;
const opened = await consoleRequest("/api/console", "POST", {});
opened.session.toUpperCase();
// @ts-expect-error Session creation does not provide output.
opened.data;
const sent = await consoleRequest("/api/console", "PUT", { data: "eA==" });
/** @type {true} */
const acknowledged = sent.ok;
void acknowledged;
validateConsoleResponse(null, "GET");
// @ts-expect-error Unsupported operations must not enter the console boundary.
await consoleRequest("/api/console", "PATCH");
