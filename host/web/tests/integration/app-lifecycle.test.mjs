import assert from "node:assert/strict";
import test from "node:test";
import { appFixture } from "../support/app-fixture.mjs";

const settle = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
};
const reply = body => ({ ok: true, status: 200, json: async () => body });
const benchmarkReady = async ({ connected, state }) => {
  connected.filesystem_format = "exfat";
  connected.memory = { initialized: true };
  connected.self_test = { sequence: 4, active: false, state: "idle" };
  await state.tick();
};

test("mounted Re-sync locks selection, preserves failure, and resets disconnected UI once", async context => {
  for (const success of [true, false]) {
    await context.test(success ? "confirmed result" : "known refusal", async context => {
      const { state, connected, elements, handlers } = await appFixture(context);
      connected.command_outcome_uncertain = true;
      connected.command_resync_supported = true;
      await state.tick();
      const pending = deferred();
      state.resyncGate = pending.promise;
      const work = handlers.get("#command-resync:click")();
      assert.equal(elements.get("#board-select").disabled, true);
      assert.equal(elements.get("#rescan-boards").disabled, true);
      elements.get("#board-select").value = "board-b";
      await handlers.get("#board-select:change")();
      assert.equal(elements.get("#board-select").value, "board-a");
      if (!success) state.resyncError = new Error("Could not confirm result");
      pending.resolve();
      await work;
      assert.equal(elements.get("#board-select").disabled, false);
      assert.equal(elements.get("#command-resync").hidden, success);
      if (!success) assert.match(elements.get("#status-message").textContent, /Could not confirm/);
      connected.connected = false;
      await state.tick();
      globalThis.document.querySelector("#wifi-status").textContent = "retained disconnected feedback";
      let clears = 0;
      elements.get("#wifi-networks").replaceChildren = () => { clears++; };
      await state.tick();
      await state.tick();
      assert.equal(clears, 0);
      assert.equal(elements.get("#wifi-status").textContent, "retained disconnected feedback");
    });
  }
});

test("all benchmark render paths and handlers enforce unresolved-command and status-error gates", async context => {
  for (const failure of ["uncertain", "status", "reboot", "wireless", "resync"]) {
    await context.test(failure, async context => {
      const fixture = await appFixture(context);
      const { state, connected, handlers, elements, requestedUrls } = fixture;
      await benchmarkReady(fixture);
      if (failure === "uncertain") {
        state.ledTimeout = true;
        await handlers.get("#led-control:click")();
        await settle();
      } else if (failure === "status") {
        state.statusFails = true;
        await state.tick();
      } else if (failure === "wireless") {
        connected.hardware.capabilities |= 128;
        connected.storage_stale = false;
        await state.tick();
        await settle();
        fixture.configureRadio("wifi");
        const pending = deferred();
        state.responses.set("/api/wireless/scan", () => pending.promise);
        const scanning = handlers.get("#wifi-scan:click")();
        elements.get("#self-test-size").valueAsNumber = 64;
        await handlers.get("#self-test-size:input")();
        assert.equal(elements.get("#self-test-start").disabled, true);
        const count = requestedUrls.length;
        await handlers.get("#self-test-start:click")();
        assert.equal(requestedUrls.length, count);
        pending.resolve(reply({ networks: [] }));
        await scanning;
        return;
      } else if (failure === "resync") {
        connected.command_outcome_uncertain = true;
        connected.command_resync_supported = true;
        await state.tick();
        const pending = deferred();
        state.resyncGate = pending.promise;
        const resyncing = handlers.get("#command-resync:click")();
        await handlers.get("#self-test-size:input")();
        assert.equal(elements.get("#self-test-start").disabled, true);
        const count = requestedUrls.length;
        await handlers.get("#self-test-start:click")();
        assert.equal(requestedUrls.length, count);
        pending.resolve();
        await resyncing;
        return;
      } else {
        await handlers.get("#reboot-control:click")();
        await handlers.get("#confirm-reboot:click")();
      }
      assert.equal(elements.get("#self-test-start").disabled, true);
      elements.get("#self-test-size").valueAsNumber = 64;
      await handlers.get("#self-test-size:input")();
      assert.equal(elements.get("#self-test-start").disabled, true);
      const count = requestedUrls.filter(url => url.startsWith("/api/self-test")).length;
      await handlers.get("#self-test-start:click")();
      connected.self_test = { sequence: 4, active: true, state: "writing" };
      if (!state.statusFails) await state.tick();
      await handlers.get("#self-test-size:input")();
      await handlers.get("#self-test-cancel:click")();
      assert.equal(requestedUrls.filter(url => url.startsWith("/api/self-test")).length, count);
    });
  }
});

test("acknowledged rebooting operations remain gated across old polls and board switches", async context => {
  for (const operation of ["save", "reboot", "resync"]) {
    await context.test(operation, async context => {
      const fixture = await appFixture(context);
      const { state, connected, handlers, elements, requestedUrls, saveAndConfirm, switchBoard } = fixture;
      await benchmarkReady(fixture);
      connected.hardware.capabilities |= 128;
      connected.storage_stale = false;
      await state.tick();
      await settle();
      if (operation === "save") {
        elements.get("#settings-clock").value = "250";
        await handlers.get("#settings-clock:change")();
        await saveAndConfirm();
      } else if (operation === "resync") {
        connected.command_outcome_uncertain = true;
        connected.command_resync_supported = true;
        state.resyncResult.control_result = { status: "completed", opcode: 5 };
        await state.tick();
        await handlers.get("#command-resync:click")();
      } else {
        await handlers.get("#reboot-control:click")();
        await handlers.get("#confirm-reboot:click")();
      }
      for (let poll = 0; poll < 2; poll++) await state.tick();
      for (const id of ["#led-control", "#msc-control", "#self-test-start", "#settings-save", "#reboot-control"])
        assert.equal(elements.get(id).disabled, true, id);
      const count = requestedUrls.filter(url => url.startsWith("/api/led")).length;
      await handlers.get("#led-control:click")();
      assert.equal(requestedUrls.filter(url => url.startsWith("/api/led")).length, count);
      const wirelessCount = requestedUrls.filter(url => url.startsWith("/api/wireless/")).length;
      await handlers.get("#wifi-scan:click")();
      elements.get("#wifi-ssid").value = "network";
      await handlers.get("#wifi-connect:click")();
      assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/")).length, wirelessCount);
      await switchBoard("board-b");
      assert.equal(elements.get("#led-control").disabled, false, "local gate belongs only to board A");
      await switchBoard("board-a");
      assert.equal(elements.get("#led-control").disabled, true);
      await fixture.reconnect();
      assert.equal(elements.get("#led-control").disabled, false);
    });
  }
});

test("successful save readback mismatch stays unresolved and does not replay", async context => {
  const { state, handlers, elements, savedSettings, saveAndConfirm, switchBoard, connected } = await appFixture(context);
  state.readbackMismatch = true;
  elements.get("#settings-clock").value = "250";
  await handlers.get("#settings-clock:change")();
  await saveAndConfirm();
  await settle();
  assert.match(elements.get("#settings-note").textContent, /outcome is uncertain/);
  for (let poll = 0; poll < 2; poll++) await state.tick();
  await saveAndConfirm();
  assert.equal(savedSettings.length, 1);
  assert.equal(elements.get("#led-control").disabled, true);
  await switchBoard("board-b");
  await switchBoard("board-a");
  await saveAndConfirm();
  assert.equal(savedSettings.length, 1);
  connected.command_outcome_uncertain = true;
  connected.command_resync_supported = true;
  await state.tick();
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#led-control").disabled, true, "Re-sync cannot erase the confirmed reboot effect");
});

test("malformed mutation success bodies are unresolved, not safe retry signals", async context => {
  for (const mutation of ["led", "settings", "reboot", "msc", "self-test"]) {
    for (const invalid of ["json", "shape"]) {
      await context.test(`${mutation}: ${invalid}`, async context => {
        const fixture = await appFixture(context);
        const { state, handlers, elements, requestedUrls } = fixture;
        await benchmarkReady(fixture);
        state.responses.set(`/api/${mutation}`, () => invalid === "json"
          ? { ok: true, status: 200, json: async () => { throw new SyntaxError("bad JSON"); } }
          : reply({ ok: false }));
        const execute = async () => {
          if (mutation === "led") await handlers.get("#led-control:click")();
          if (mutation === "self-test") await handlers.get("#self-test-start:click")();
          if (mutation === "settings") {
            elements.get("#settings-clock").value = "250";
            await handlers.get("#settings-clock:change")();
            await fixture.saveAndConfirm();
          }
          if (mutation === "reboot") {
            await handlers.get("#reboot-control:click")();
            await handlers.get("#confirm-reboot:click")();
          }
          if (mutation === "msc") {
            await handlers.get("#msc-control:click")();
            await handlers.get("#confirm-share:click")();
          }
        };
        await execute();
        await settle();
        const count = () => requestedUrls.filter(url => url.startsWith(`/api/${mutation}?`)).length;
        assert.equal(count(), 1);
        assert.equal(elements.get(mutation === "msc" ? "#msc-control" : "#led-control").disabled, true);
        await execute();
        assert.equal(count(), 1, "an unusable success reply never authorizes replay");
      });
    }
  }
});

test("malformed Re-sync cannot clear server uncertainty", async context => {
  const { state, connected, handlers, elements } = await appFixture(context);
  connected.command_outcome_uncertain = true;
  connected.command_resync_supported = true;
  await state.tick();
  state.resyncResult = {};
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#command-resync").hidden, false);
  assert.equal(elements.get("#led-control").disabled, true);
  assert.match(elements.get("#status-message").textContent, /did not establish/);
});

test("unknown-board discovery uses the production 404 and the exact five-second retry limit", async context => {
  const { state, requestedUrls, elements } = await appFixture(context);
  const scans = () => requestedUrls.filter(url => url === "/api/boards").length;
  const initial = scans();
  state.statusUnknownBoard = true;
  await state.tick();
  await settle();
  assert.equal(scans(), initial + 1);
  state.now += 4999;
  await state.tick();
  await settle();
  assert.equal(scans(), initial + 1);
  state.now++;
  await state.tick();
  await settle();
  assert.equal(scans(), initial + 2);
  state.statusUnknownBoard = false;
  await state.tick();
  assert.equal(elements.get("#led-control").disabled, false);
  assert.equal(elements.get("#board-select").value, "board-a");
});

test("known command refusals remain explicitly retryable without waiting for reconnection", async context => {
  for (const mutation of ["led", "self-test"]) {
    await context.test(mutation, async context => {
      const fixture = await appFixture(context);
      const { state, handlers, requestedUrls, elements } = fixture;
      await benchmarkReady(fixture);
      state.responses.set(`/api/${mutation}`, () => ({
        ok: false, status: 409, json: async () => ({ error: "Board is busy", uncertain: false }),
      }));
      const button = mutation === "led" ? "#led-control" : "#self-test-start";
      await handlers.get(`${button}:click`)();
      assert.equal(elements.get(button).disabled, false);
      await handlers.get(`${button}:click`)();
      assert.equal(requestedUrls.filter(url => url.startsWith(`/api/${mutation}?`)).length, 2);
    });
  }
});

test("wireless mutation contract failures retain uncertainty and prevent replay", async context => {
  for (const action of ["scan", "test", "cancel", "save"]) {
    await context.test(action, async context => {
      const fixture = await appFixture(context);
      const { connected, state, handlers, elements, requestedUrls, configureRadio } = fixture;
      connected.hardware.capabilities |= 128;
      connected.storage_stale = false;
      await state.tick();
      await settle();
      configureRadio("wifi");
      elements.get("#wifi-ssid").value = "network";
      if (["cancel", "save"].includes(action)) await handlers.get("#wifi-connect:click")();
      const path = `/api/wireless${action === "save" ? "" : `/${action}`}`;
      state.responses.set(path, () => reply({}));
      const perform = () => action === "scan" ? handlers.get("#wifi-scan:click")()
        : action === "test" ? handlers.get("#wifi-connect:click")()
          : action === "cancel" ? handlers.get("#wifi-cancel:click")() : fixture.saveAndConfirm();
      await perform();
      await settle();
      const count = () => requestedUrls.filter(url => url.startsWith(`${path}?`)).length;
      assert.equal(count(), 1);
      assert.equal(elements.get("#led-control").disabled, true);
      await perform();
      assert.equal(count(), 1);
    });

  }
});

test("known server cleanup failures offer explicit retry without another test or Re-sync", async context => {
  const { connected, state, handlers, elements, requestedUrls } = await appFixture(context);
  connected.hardware.capabilities |= 128;
  connected.storage_stale = false;
  connected.wireless_cleanup_required = true;
  await state.tick();
  await settle();
  assert.equal(elements.get("#command-resync").hidden, false);
  assert.equal(elements.get("#command-resync").textContent, "Clear Wi-Fi test");
  assert.equal(elements.get("#board-select").disabled, false);
  assert.match(elements.get("#status-message").textContent, /credentials need cleanup/);
  for (const id of ["#led-control", "#settings-save", "#reboot-control", "#msc-control"])
    assert.equal(elements.get(id).disabled, true, id);
  const before = requestedUrls.length;
  await handlers.get("#led-control:click")();
  await handlers.get("#wifi-scan:click")();
  assert.equal(requestedUrls.length, before);
  state.responses.set("/api/wireless/cancel", () => ({
    ok: false, status: 409, json: async () => ({ error: "Cleanup failed", uncertain: false }),
  }));
  await handlers.get("#command-resync:click")();
  assert.match(elements.get("#wireless-summary").textContent, /Cleanup failed/);
  assert.equal(elements.get("#command-resync").disabled, false);
  state.responses.set("/api/wireless/cancel", () => {
    connected.wireless_cleanup_required = false;
    return reply({ cancelled: true });
  });
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#command-resync").hidden, true);
  assert.equal(elements.get("#led-control").disabled, false);
  assert.equal(elements.get("#wireless-summary").hidden, true);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/cancel?")).length, 2);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/test?")).length, 0);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/control/resync?")).length, 0);
});

test("unknown cleanup result requires Re-sync and a confirmed cancel clears local recovery state", async context => {
  const { connected, state, handlers, elements, requestedUrls } = await appFixture(context);
  connected.hardware.capabilities |= 128;
  connected.storage_stale = false;
  connected.wireless_cleanup_required = true;
  await state.tick();
  state.responses.set("/api/wireless/cancel", () => {
    connected.command_outcome_uncertain = true;
    connected.command_resync_supported = true;
    throw new TypeError("lost reply");
  });
  await handlers.get("#command-resync:click")();
  await state.tick();
  assert.equal(elements.get("#command-resync").textContent, "Re-sync");
  assert.equal(elements.get("#led-control").disabled, true);
  state.resyncResult.control_result = { status: "completed", opcode: 10 };
  state.responses.set("/api/control/resync", () => {
    connected.command_outcome_uncertain = false;
    connected.wireless_cleanup_required = false;
    return reply(state.resyncResult);
  });
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#command-resync").hidden, true);
  assert.equal(elements.get("#led-control").disabled, false);
  assert.equal(elements.get("#wireless-summary").hidden, true);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/cancel?")).length, 1);
});

test("Re-sync of a failed cancel allows only an explicit cleanup retry", async context => {
  const { connected, state, handlers, elements, requestedUrls } = await appFixture(context);
  connected.hardware.capabilities |= 128;
  connected.storage_stale = false;
  connected.wireless_cleanup_required = true;
  await state.tick();
  state.responses.set("/api/wireless/cancel", () => {
    connected.command_outcome_uncertain = true;
    connected.command_resync_supported = true;
    throw new TypeError("lost reply");
  });
  await handlers.get("#command-resync:click")();
  await state.tick();
  state.resyncResult.control_result = { status: "failed", opcode: 10 };
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#command-resync").textContent, "Clear Wi-Fi test");
  assert.equal(elements.get("#led-control").disabled, true);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/cancel?")).length, 1);
  state.responses.set("/api/wireless/cancel", () => {
    connected.wireless_cleanup_required = false;
    return reply({ cancelled: true });
  });
  await handlers.get("#command-resync:click")();
  assert.equal(elements.get("#led-control").disabled, false);
  assert.equal(requestedUrls.filter(url => url.startsWith("/api/wireless/cancel?")).length, 2);
});
