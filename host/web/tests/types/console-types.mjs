import { consoleRequest, validateConsoleResponse } from "../../js/console-api.mjs";

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
