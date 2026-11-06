import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import test from "node:test";
import { domFixture } from "../support/dom-fixture.mjs";

const source = readFileSync(new URL("../../src/js/app.mjs", import.meta.url), "utf8");
const settle = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
};

test("Re-sync fences success and failure after board generation changes", async context => {
  for (const success of [true, false]) {
    await context.test(success ? "late success" : "late failure", async () => {
      const pending = deferred();
      let updates = 0;
      const sandbox = vm.createContext({
        currentSnapshot: { command_outcome_uncertain: true }, selectionVersion: 1,
        commandResyncInProgress: false, renderStatus() {}, updateBoardSelectionControls() {},
        boardUrl: path => path, fetchWithDeadline: () => pending.promise, responseJson: async value => value,
        saveBoardCommandState() { updates++; }, updateStatus: async () => { updates++; },
      });
      const start = source.indexOf("async function resyncCommandOutcome()");
      const end = source.indexOf("\nasync function updateStatus()", start);
      vm.runInContext(source.slice(start, end), sandbox);
      const work = sandbox.resyncCommandOutcome();
      sandbox.selectionVersion++;
      const current = { command_outcome_uncertain: true, error: "Board B pending", wireless: { ssid: "Board B" } };
      sandbox.currentSnapshot = current;
      if (success) pending.resolve({ wireless: { ssid: "Board A" } });
      else pending.reject(new Error("Board A failed"));
      await work;
      assert.deepEqual(current, { command_outcome_uncertain: true, error: "Board B pending", wireless: { ssid: "Board B" } });
      assert.equal(updates, 0);
    });
  }
});

test("cancellation handles delivery uncertainty and ignores errors from prior board generations", async context => {
  for (const error of [new DOMException("deadline", "TimeoutError"), new TypeError("network failure"),
    new Error("known refusal")]) {
    for (const stale of [true, false]) {
      await context.test(`${error.name}, ${stale ? "old" : "current"} board`, async () => {
        const pending = deferred();
        const sandbox = vm.createContext({
          selectionVersion: 1, uncertainCommand: null, boardUrl: path => path,
          neverSettles: () => false, fetchWithDeadline: () => pending.promise, responseJson: async value => value,
          hasUnknownOutcome: error => ["TimeoutError", "TypeError"].includes(error.name),
        });
        const start = source.indexOf("  cancelTest: async () => {");
        const end = source.indexOf("\n  changed:", start);
        const adapter = source.slice(start, end).trim().replace(/^cancelTest: /, "").replace(/,$/, "");
        const work = vm.runInContext(`(${adapter})()`, sandbox);
        if (stale) sandbox.selectionVersion++;
        pending.reject(error);
        await assert.rejects(work, error);
        assert.equal(sandbox.uncertainCommand !== null, !stale && error.name !== "Error");
      });
    }
  }
});

test("mounted dashboard locks board selection during Re-sync and resets disconnected UI once", async context => {
  const fixture = domFixture();
  const saved = { document: globalThis.document, window: globalThis.window };
  let tick;
  Object.assign(globalThis, {
    document: fixture.document, window: { setInterval: callback => { tick = callback; } },
  });
  context.after(() => Object.assign(globalThis, saved));
  const pending = deferred();
  let connected = true;
  let resyncing = false;
  context.mock.method(globalThis, "fetch", async url => {
    if (url === "/api/boards") return { ok: true, json: async () => ({
      boards: [{ id: "board-a", serial_number: "A" }, { id: "board-b", serial_number: "B" }],
      default_board_id: "board-a",
    }) };
    if (url.startsWith("/api/control/resync")) { resyncing = true; return pending.promise; }
    return { ok: true, json: async () => ({ connected, command_outcome_uncertain: connected,
      command_resync_supported: true, hardware: { capabilities: 2 }, memory: { msc: { enabled: false } } }) };
  });
  await import(`../../src/js/app.mjs?lifecycle=${Date.now()}`);
  await settle();
  const work = fixture.el("#command-resync").fire("click");
  assert.equal(resyncing, true);
  assert.equal(fixture.el("#board-select").disabled, true);
  assert.equal(fixture.el("#rescan-boards").disabled, true);
  fixture.el("#board-select").value = "board-b";
  await fixture.el("#board-select").fire("change");
  assert.equal(fixture.el("#board-select").value, "board-a", "programmatic board selection is also rejected");
  pending.resolve({ ok: true, json: async () => ({}) });
  await work;
  assert.equal(fixture.el("#board-select").disabled, false);
  connected = false;
  await tick();
  fixture.el("#wifi-status").textContent = "retained disconnected feedback";
  let clears = 0;
  fixture.el("#wifi-networks").replaceChildren = () => { clears++; };
  await tick();
  await tick();
  assert.equal(clears, 0, "disconnected polling does not repeatedly wipe the wireless UI");
  assert.equal(fixture.el("#wifi-status").textContent, "retained disconnected feedback");
});
