// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { bindWirelessForm, formTransition } from "../../../web/js/ui/sections/wireless-form.mjs";
import { createDocument } from "../../support/dom.mjs";
import { OP } from "../../../web/js/protocol/constants.mjs";

test("failed replacement-key Save can be abandoned by disabling Bluetooth", async () => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  const form = bindWirelessForm({
    supported: () => true, ready: () => true, scan: async () => [],
    test: async () => ({ remaining_seconds: 60 }), cancelTest: async () => {},
  });
  try {
    form.update({ wireless: {
      ssid: "", wifi_enabled: false, ble_enabled: true, ble_key_configured: true, tcp_cli_enabled: true,
    } });
    el("#direct-settings-ble").value = "1";
    await fire("#direct-ble-generate", "click");
    form.finish(null, "Save failed");
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), { message: form.saveBlockReason() });
    assert.equal(el("#direct-wireless-summary").textContent, form.saveBlockReason());
    await fire("#direct-settings-ble", "focus");
    el("#direct-settings-ble").value = "0";
    await fire("#direct-settings-ble", "input");
    await fire("#direct-settings-ble", "change");
    assert.equal(form.canSave(), true);
    assert.equal(form.saveBlockReason(), null);
    assert.equal(form.dirty(), true);
    assert.equal(form.request().ble_key, "");
    assert.equal(el("#direct-ble-secret").value, "");
  } finally { form.dispose(); globalThis.document = previous; }
});

test("dashboard form updates reuse gates but local input refreshes current readiness", async () => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  let checks = 0, allowed = true;
  const ready = () => { checks++; return allowed; };
  const form = bindWirelessForm({
    supported: () => true, ready, settingsReady: ready, tcpReady: ready,
    scan: async () => [], test: async () => ({ remaining_seconds: 599 }), cancelTest: async () => {},
  });
  try {
    form.update({ wireless: { ssid: "saved", tcp_cli_enabled: true } },
      { wireless: true, settings: true, tcp: true });
    assert.equal(checks, 0, "dashboard-provided gates are reused");
    allowed = false;
    await fire("#direct-wifi-ssid", "input");
    assert.equal(checks, 3, "local refresh evaluates each live callback once");
    assert.equal(el("#direct-wifi-scan").disabled, true);
    assert.equal(el("#direct-settings-tcp-cli").disabled, true);
  } finally { form.dispose(); globalThis.document = previous; }
});

test("replacement tests invalidate the previous pair and require retry or cancellation after failure", async () => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  let fails = false;
  let cancelCount = 0;
  const form = bindWirelessForm({
    supported: () => true, ready: () => true, scan: async () => [],
    test: async () => {
      if (fails) throw new Error("Replacement failed");
      return { remaining_seconds: 599 };
    },
    cancelTest: async () => { cancelCount++; },
  });
  try {
    el("#direct-settings-wifi").value = "1";
    form.update({ wireless: { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true } });
    el("#direct-wifi-ssid").value = "first";
    await fire("#direct-wifi-connect", "click");
    assert.equal(form.request().ssid, "first");
    assert.equal(form.request().use_tested_wifi, true);

    fails = true;
    el("#direct-wifi-ssid").value = "replacement";
    await fire("#direct-wifi-connect", "click");
    assert.match(el("#direct-wifi-status").textContent, /Replacement failed/);
    assert.equal(form.canSave(), false);
    assert.equal(form.dirty(), true, "firmware staging still needs clearing");
    assert.throws(() => form.request(), /again or clear/);
    assert.match(el("#direct-wireless-summary").textContent, /again or clear/, "failed staging explains Save blocker");

    fails = false;
    await fire("#direct-wifi-connect", "click");
    assert.equal(form.canSave(), true);
    assert.equal(form.request().ssid, "replacement");
    fails = true;
    await fire("#direct-wifi-connect", "click");
    el("#direct-settings-wifi").focus();
    el("#direct-settings-wifi").value = "0";
    await fire("#direct-settings-wifi", "input");
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(cancelCount, 1, "disabling also clears a failed replacement test");
    assert.equal(form.canSave(), true);
    assert.equal(Object.hasOwn(form.request(), "use_tested_wifi"), false);
  } finally { form.dispose(); globalThis.document = previous; }
});

test("scan selection retains raw SSID bytes, manual edits drop hex, "
  + "and failed-test cancellation clears staging", async () => {
  const previous = globalThis.document;
  const { document, element, fire } = createDocument();
  const el = id => element(`#${id}`);
  globalThis.document = document;
  let cancelCount = 0, cancellationFails = false, testFails = false;
  const tested = [];
  const form = bindWirelessForm({
    supported: () => true, ready: () => true,
    scan: async () => [{ ssid: "\\x00\\xFF<script>", ssid_hex: "00ff3c7363726970743e", rssi: -60, secured: false }],
    test: async request => {
      tested.push(request);
      if (testFails) throw new Error("Password rejected");
      return { ipv4: "192.168.1.2", remaining_seconds: 599 };
    },
    cancelTest: async () => { cancelCount++; if (cancellationFails) throw new Error("clear failed"); },
  });
  try {
    el("direct-settings-wifi").value = "1"; el("direct-settings-ble").value = "0";
    form.update({ wireless: { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true, ble_enabled: false } });
    await fire("#direct-wifi-scan", "click");
    const button = el("direct-wifi-networks").children[0];
    assert.equal(button.role, undefined, "network choices use native buttons, not a partial listbox");
    assert.equal(button["aria-pressed"], "false");
    assert.equal(button.children[0].textContent, "\\x00\\xFF<script>", "SSID label is text, never HTML");
    button.dispatchEvent(new Event("click"));
    assert.equal(button["aria-pressed"], "true", "selection state follows the selected raw SSID");
    await fire("#direct-wifi-connect", "click");
    assert.equal(tested[0].ssid_hex, "00ff3c7363726970743e");
    assert.equal(form.request().ssid_hex, "00ff3c7363726970743e");
    assert.equal(form.request().use_tested_wifi, true);
    cancellationFails = true;
    await fire("#direct-wifi-cancel", "click");
    assert.equal(form.canSave(), false, "failed discard cannot silently lose the retained pair");
    cancellationFails = false;
    await fire("#direct-wifi-cancel", "click");
    assert.equal(form.canSave(), true);
    assert.equal(cancelCount, 2);
    el("direct-wifi-ssid").value = "manual"; await fire("#direct-wifi-ssid", "input");
    assert.equal(button["aria-pressed"], "false", "manual edits clear the scanned selection");
    testFails = true;
    await fire("#direct-wifi-connect", "click");
    assert.equal(Object.hasOwn(tested[1], "ssid_hex"), false, "manual text is encoded exactly once");
    await fire("#direct-wifi-cancel", "click");
    assert.equal(cancelCount, 3, "a failed test also leaves firmware staging that must be discarded");
  } finally { form.reset(); globalThis.document = previous; }
});

test("TCP switch is saved explicitly and expired test credentials block saving", async context => {
  const previous = globalThis.document;
  const { document, element, fire } = createDocument();
  const el = id => element(`#${id}`);
  globalThis.document = document;
  let now = 100;
  const intervals = new Map();
  let nextTimer = 0;
  const timerScope = {
    createChild() { return this; },
    setInterval(callback) { const handle = ++nextTimer; intervals.set(handle, callback); return handle; },
    clearInterval(handle) { intervals.delete(handle); },
    dispose() { intervals.clear(); },
  };
  const tick = () => { for (const callback of intervals.values()) callback(); };
  let notifications = 0, saveEnabled = false;
  const previousCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  context.mock.method(Date, "now", () => now);
  let status = { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true,
    ble_key_configured: false };
  let supported = true, ready = true;
  try {
    el("direct-settings-wifi").value = "1";
    el("direct-settings-ble").value = "0";
    const form = bindWirelessForm({ ready: () => ready, supported: () => supported,
      timerScope, changed: () => { notifications++; saveEnabled = form.canSave(); },
      test: async () => ({ ipv4: "192.168.1.2", remaining_seconds: 600 }), cancelTest: async () => {} });
    form.update({ wireless: status });
    assert.equal(form.dirty(), false);
    assert.equal(el("direct-settings-tcp-cli").disabled, false);
    assert.equal(el("direct-settings-tcp-cli").label.hidden, false);
    el("direct-settings-tcp-cli").value = "0";
    await fire("#direct-settings-tcp-cli", "change");
    form.update({ wireless: status });
    assert.equal(el("direct-settings-tcp-cli").value, "0", "periodic refresh preserves unsaved TCP edits");
    assert.equal(form.dirty(), true);
    assert.equal(form.request().tcp_cli_enabled, false);
    form.finish({ ...status, tcp_cli_enabled: false });
    assert.equal(form.dirty(), false);
    const bleOn = () => el("direct-settings-ble").options.find(option => option.value === "1");
    assert.equal(bleOn().disabled, true, "Bluetooth cannot be enabled without a saved or staged key");
    Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues: bytes => bytes.fill(1) } });
    await fire("#direct-ble-generate", "click");
    assert.equal(bleOn().disabled, false);
    el("direct-settings-tcp-cli").value = "1";
    await fire("#direct-settings-tcp-cli", "change");
    assert.equal(el("direct-settings-ble").value, "1", "generating a key enables Bluetooth");
    assert.equal(Object.hasOwn(form.request(), "tcp_cli_auth_required"), false, "TCP auth follows the key");
    el("direct-settings-ble").value = "0";
    form.update();
    assert.equal(el("direct-settings-tcp-cli").disabled, false, "TCP CLI only depends on Wi-Fi");
    ready = false;
    form.update();
    assert.equal(el("direct-settings-tcp-cli").disabled, true);
    ready = true;
    form.update();
    el("direct-wifi-ssid").value = "tested";
    await fire("#direct-wifi-connect", "click");
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(form.canSave(), true);
    assert.equal(saveEnabled, true);
    const beforeExpiry = notifications;
    now += 599000;
    tick();
    assert.equal(notifications, beforeExpiry, "countdown ticks do not rerender the dashboard");
    assert.match(el("direct-wireless-summary").textContent, /expires in 1s/);
    now += 1000;
    tick();
    assert.equal(form.canSave(), false);
    assert.equal(saveEnabled, false, "expiry notifies Save readiness without any board or form update");
    assert.equal(notifications, beforeExpiry + 1, "expiry produces one dashboard notification");
    assert.equal(intervals.size, 0, "the completed expiry countdown stops");
    tick();
    assert.equal(notifications, beforeExpiry + 1, "expired credentials do not repeatedly notify");
    assert.throws(() => form.request(), /expired/);
    assert.match(el("direct-wireless-summary").textContent, /expired/);
    el("direct-settings-wifi").value = "0";
    form.update();
    assert.equal(el("direct-settings-tcp-cli").disabled, true);
    assert.equal(el("direct-settings-tcp-cli").value, "1");
    assert.equal(form.canSave(), true, "expired credentials do not prevent disabling Wi-Fi");
    const disabled = form.request();
    assert.equal(disabled.wifi_enabled, false);
    assert.equal(disabled.password, "");
    assert.equal(disabled.ssid, "");
    assert.equal(Object.hasOwn(disabled, "use_tested_wifi"), false);
    assert.equal(el("direct-wireless-summary").hidden, true);
    form.reset();
    form.update();
    supported = false;
    form.update();
    for (const id of ["direct-settings-tcp-cli"]) {
      assert.equal(el(id).label.hidden, true, "non-wireless boards hide TCP controls");
      assert.equal(el(id).disabled, true);
    }
  } finally {
    globalThis.document = previous;
    if (previousCrypto) Object.defineProperty(globalThis, "crypto", previousCrypto);
    else delete globalThis.crypto;
  }
});

test("one form phase serializes scan, test and cancel, and a reset drops late results", async () => {
  const previous = globalThis.document;
  const { document, element, fire } = createDocument();
  const el = id => element(`#${id}`);
  globalThis.document = document;
  const deferred = () => {
    let resolve, reject;
    const promise = new Promise((ok, fail) => { resolve = ok; reject = fail; });
    return { promise, resolve, reject };
  };
  let scan, check, cancel;
  try {
    el("direct-settings-wifi").value = "1";
    el("direct-settings-ble").value = "0";
    const form = bindWirelessForm({ ready: () => true, supported: () => true,
      scan: () => (scan = deferred()).promise, test: () => (check = deferred()).promise,
      cancelTest: () => (cancel = deferred()).promise });
    form.update({ wireless: { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true } });

    const scanning = fire("#direct-wifi-scan", "click");
    assert.equal(form.operationBusy(), true, "scanning owns the device");
    assert.equal(form.canSave(), false, "a running operation cannot produce a save request");
    assert.equal(el("direct-wifi-connect").disabled, true);
    form.reset();
    assert.equal(form.operationBusy(), false, "reset returns to idle");
    scan.resolve([{ ssid: "late", ssid_hex: "6c617465", rssi: -40, secured: true }]);
    await scanning;
    assert.equal(el("direct-wifi-networks").children.length, 0, "a scan from before the reset is ignored");
    assert.equal(form.operationBusy(), false);

    form.update({ wireless: { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true } });
    el("direct-wifi-ssid").value = "home";
    el("direct-wifi-password").value = "password";
    const testing = fire("#direct-wifi-connect", "click");
    assert.equal(el("direct-wifi-connect").textContent, "Testing…");
    assert.equal(el("direct-wifi-cancel").disabled, true, "cancel waits for the test result");
    check.reject(new Error("Password rejected"));
    await testing;
    assert.equal(el("direct-wifi-connect").textContent, "Test connection");
    assert.equal(form.operationBusy(), false);
    assert.equal(form.dirty(), true, "attempted but untested firmware staging remains owned by the form");
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /again or clear/);

    const canceling = fire("#direct-wifi-cancel", "click");
    assert.equal(el("direct-wifi-cancel").textContent, "Clearing…");
    assert.equal(form.canSave(), false);
    cancel.reject(new Error("link lost"));
    await canceling;
    assert.equal(el("direct-wifi-cancel").textContent, "Retry clearing test");
    assert.equal(form.operationBusy(), true, "an uncleared test blocks other device work");
    assert.throws(() => form.request(), /Clear the temporary Wi-Fi test/);
    const retry = fire("#direct-wifi-cancel", "click");
    cancel.resolve();
    await retry;
    assert.equal(el("direct-wifi-cancel").textContent, "Cancel");
    assert.equal(form.operationBusy(), false);
    assert.equal(form.canSave(), true);
    assert.equal(form.dirty(), false, "known cancellation releases all temporary staging");
    assert.equal(form.request(), null, "the canceled pair cannot leak into a later save request");
    assert.equal(el("direct-wifi-ssid").value, "");
    assert.equal(el("direct-wifi-password").value, "");
    form.dispose();
  } finally { globalThis.document = previous; }
});

test("reset fences successful and failed in-flight Wi-Fi tests and their progress callbacks", async () => {
  const previous = globalThis.document;
  try {
    for (const outcome of ["resolve", "reject"]) {
      const { document, element: el, fire } = createDocument();
      globalThis.document = document;
      let resolveTest, rejectTest, progress, notifications = 0;
      const form = bindWirelessForm({
        supported: () => true, ready: () => true, scan: async () => [], cancelTest: async () => {},
        changed: () => notifications++,
        test: (_, onProgress) => {
          progress = onProgress;
          return new Promise((resolve, reject) => { resolveTest = resolve; rejectTest = reject; });
        },
      });
      try {
        el("#direct-settings-wifi").value = "1";
        form.update({ wireless: { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true } });
        el("#direct-wifi-ssid").value = "old test";
        const testing = fire("#direct-wifi-connect", "click");
        const originalSignal = form.signal;
        form.reset();
        assert.equal(originalSignal.aborted, true);
        assert.notEqual(form.signal, originalSignal);
        form.update({ wireless: { ssid: "new saved", wifi_enabled: true, tcp_cli_enabled: true } });
        el("#direct-wifi-status").textContent = "new lifecycle";
        el("#direct-wifi-dialog").open = true;
        const before = notifications;
        progress("old progress");
        if (outcome === "resolve") resolveTest({ remaining_seconds: 599 });
        else rejectTest(new Error("Old test failed"));
        await testing;
        assert.equal(el("#direct-wifi-status").textContent, "new lifecycle", outcome);
        assert.equal(el("#direct-wifi-dialog").open, true, outcome);
        assert.equal(el("#direct-wifi-ssid").value, "new saved", outcome);
        assert.equal(notifications, before, outcome);
        assert.equal(form.operationBusy(), false);
        assert.equal(form.canSave(), true);
        assert.equal(form.dirty(), false);
        assert.equal(form.request(), null, "obsolete test credentials are never offered to Save");
      } finally { form.dispose(); }
    }
  } finally { globalThis.document = previous; }
});

test("known cancellation recovery clears the staged pair without replaying cancellation", async () => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  let ready = true, cancellations = 0;
  const form = bindWirelessForm({
    supported: () => true, ready: () => ready, scan: async () => [],
    test: async () => ({ remaining_seconds: 599 }),
    cancelTest: async () => { cancellations++; ready = false; throw new Error("Link lost"); },
  });
  try {
    el("#direct-settings-wifi").value = "1";
    form.update({ wireless: { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true } });
    el("#direct-wifi-ssid").value = "tested";
    await fire("#direct-wifi-connect", "click");
    await fire("#direct-wifi-cancel", "click");
    assert.equal(form.canSave(), false);
    assert.equal(form.operationBusy(), true);
    ready = true;
    form.update({ connected: true, outcome_uncertain: false, recovered_opcode: OP.CANCEL_TEST });
    assert.equal(cancellations, 1, "reconciliation never resends a known cancellation");
    assert.equal(form.operationBusy(), false);
    assert.equal(form.canSave(), true);
    assert.equal(form.dirty(), false);
    assert.equal(form.request(), null);
    assert.equal(el("#direct-wifi-ssid").value, "");
    assert.match(el("#direct-wifi-status").textContent, /cleared after reconnect/);
  } finally { form.dispose(); globalThis.document = previous; }
});

test("formTransition serializes one phase and requires reconnect recovery before a retry", () => {
  const step = (form, ...events) => events.reduce((value, type) =>
    formTransition(value, typeof type === "string" ? { type } : type), form);
  const idle = step({}, "reset");
  assert.equal(idle.phase, "idle");
  assert.equal(step(idle, "scan", "test").phase, "scanning", "a running scan rejects a test");
  assert.equal(step(idle, "scan", "done").phase, "idle");
  assert.equal(step(idle, "test", "cancel").phase, "testing", "cancel waits for the test to finish");
  const failed = step(idle, "cancel", { type: "cancelFailed", error: "x", needsReconnect: true });
  assert.deepEqual(failed, { phase: "cancelFailed", cancelError: "x", cancelNeedsReconnect: true });
  assert.equal(step(failed, "cancel"), failed, "retry is refused until the board reconnects");
  assert.equal(step(failed, "recovered", "cancel", "cleared").phase, "idle");
  assert.equal(step(idle, "test", "reset").phase, "idle", "reset drops any in-flight phase");
});

test("obsolete cancellation and clipboard completions cannot mutate a reset wireless form", async () => {
  const previous = globalThis.document;
  try {
    for (const trigger of ["disable", "button", "escape"]) {
      for (const outcome of ["resolve", "reject"]) {
        const { document, element, fire } = createDocument();
        globalThis.document = document;
        const el = id => element(`#${id}`);
        let resolve, reject, resolveCopy, rejectCopy, notifications = 0;
        const form = bindWirelessForm({
          ready: () => true, supported: () => true, changed: () => notifications++,
          test: async () => ({ ipv4: "192.0.2.2", remaining_seconds: 600 }),
          cancelTest: () => new Promise((ok, fail) => { resolve = ok; reject = fail; }),
          clipboard: { writeText: () => new Promise((ok, fail) => { resolveCopy = ok; rejectCopy = fail; }) },
        });
        el("direct-settings-wifi").value = "1";
        el("direct-settings-ble").value = "0";
        form.update({ wireless: { ssid: "saved", wifi_enabled: true } });
        el("direct-wifi-ssid").value = "tested";
        await fire("#direct-wifi-connect", "click");
        const copying = fire("#direct-ble-copy", "click");
        if (trigger === "disable") {
          el("direct-settings-wifi").focus();
          el("direct-settings-wifi").value = "0";
          void fire("#direct-settings-wifi", "input");
        } else if (trigger === "button") void fire("#direct-wifi-cancel", "click");
        else void fire("#direct-wifi-dialog", "cancel");
        form.reset();
        const afterReset = notifications;
        // A newer lifecycle may already have opened its own dialog.
        el("direct-wifi-dialog").open = trigger !== "disable";
        el("direct-ble-status").textContent = "new lifecycle";
        const radio = el("direct-settings-wifi").value;
        if (outcome === "resolve") { resolve(); resolveCopy(); }
        else { reject(new Error("old connection failed")); rejectCopy(new Error("old clipboard failure")); }
        await copying;
        await new Promise(resolve => setImmediate(resolve));
        assert.equal(el("direct-wifi-dialog").open, trigger !== "disable", `${trigger}/${outcome}: dialog`);
        assert.equal(el("direct-settings-wifi").value, radio, `${trigger}/${outcome}: radio`);
        assert.equal(notifications, afterReset, `${trigger}/${outcome}: notification`);
        assert.equal(el("direct-ble-status").textContent, "new lifecycle", `${trigger}/${outcome}: clipboard`);
        assert.equal(form.operationBusy(), false);
        form.dispose();
      }
    }
  } finally { globalThis.document = previous; }
});
