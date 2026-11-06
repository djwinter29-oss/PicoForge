import assert from "node:assert/strict";
import test from "node:test";
import { bindWirelessForm, renderWirelessStatus } from "../../src/js/wireless-form.mjs";
import { domFixture } from "../support/dom-fixture.mjs";

function formFixture(context, overrides = {}) {
  const previous = globalThis.document;
  const fixture = domFixture();
  const el = id => fixture.el(`#${id}`);
  el("settings-wifi").value = "1";
  el("settings-ble").value = "0";
  globalThis.document = fixture.document;
  const form = bindWirelessForm({
    ready: () => true, supported: () => true,
    test: async () => ({ ipv4: "192.0.2.1", remaining_seconds: 600 }), cancelTest: async () => {},
    ...overrides,
  });
  form.initialize({ ssid: "saved", tcp_cli_enabled: true });
  context.after(() => { form.reset(); globalThis.document = previous; });
  return { form, el };
}

function deferred() {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

test("clipboard feedback belongs to the current key draft and board lifecycle", async context => {
  for (const change of ["current", "regenerate", "disable", "saved", "failed-save", "reset"]) {
    for (const succeeds of [true, false]) {
      await context.test(`${change}: late clipboard ${succeeds ? "success" : "failure"}`, async context => {
        const pending = deferred();
        const navigatorDescriptor = Object.getOwnPropertyDescriptor(globalThis, "navigator");
        const copied = [];
        Object.defineProperty(globalThis, "navigator", { configurable: true, value: {
          clipboard: { writeText: text => { copied.push(text); return pending.promise; } },
        } });
        context.after(() => {
          if (navigatorDescriptor) Object.defineProperty(globalThis, "navigator", navigatorDescriptor);
          else delete globalThis.navigator;
        });
        const { form, el } = formFixture(context);
        await el("ble-generate").fire("click");
        const copy = el("ble-copy").fire("click");
        assert.equal(copied.length, 1);
        if (change === "regenerate") {
          await el("ble-generate").fire("click");
          assert.notEqual(el("ble-secret").value, copied[0]);
        } else if (change === "disable") {
          el("settings-ble").value = "0";
          await el("settings-ble").fire("input");
        } else if (change === "saved") form.finish({ ssid: "saved", ble_key_configured: true });
        else if (change === "failed-save") form.finish(null, "Save failed");
        else if (change === "reset") form.reset();
        const feedback = el("ble-status").textContent;
        if (succeeds) pending.resolve();
        else pending.reject(new Error("clipboard unavailable"));
        await copy;
        if (change === "current") {
          assert.match(el("ble-status").textContent, succeeds ? /Key copied/ : /Could not copy key: clipboard unavailable/);
        } else assert.equal(el("ble-status").textContent, feedback);
        if (change === "failed-save") {
          assert.equal(feedback, "Save failed");
          assert.equal(form.canSave(), false);
          assert.throws(() => form.request(), /Generate a replacement/);
          el("settings-ble").value = "0";
          await el("settings-ble").fire("input");
          assert.equal(form.canSave(), true, "disabling releases failed-key recovery");
        }
      });
    }
  }
});

test("failed replacement Wi-Fi testing invalidates the previous stage and blocks Save until known cleanup", async context => {
  let fails = false;
  const { form, el } = formFixture(context, {
    test: async () => {
      if (fails) throw new Error("access point refused");
      return { ipv4: "192.0.2.1", remaining_seconds: 600 };
    },
  });
  await el("wifi-connect").fire("click");
  assert.equal(form.canSave(), true);
  fails = true;
  await el("wifi-connect").fire("click");
  assert.equal(form.dirty(), true);
  assert.equal(form.canSave(), false);
  assert.equal(form.stageValid(), false);
  assert.throws(() => form.request(), /Test the connection again or clear/);
  await el("wifi-cancel").fire("click");
  assert.equal(form.canSave(), true);
  assert.equal(form.dirty(), false);
});

test("reset fences wireless continuations, including errors", async context => {
  for (const operation of ["scan", "cancel", "test"]) {
    for (const succeeds of [true, false]) {
      await context.test(`${operation}: late ${succeeds ? "success" : "failure"}`, async context => {
        const pending = deferred();
        const { form, el } = formFixture(context, {
          scan: () => pending.promise, cancelTest: () => pending.promise,
          ...(operation === "test" ? { test: () => pending.promise } : {}),
        });
        if (operation === "cancel") await el("wifi-connect").fire("click");
        const work = el({ scan: "wifi-scan", cancel: "wifi-cancel", test: "wifi-connect" }[operation]).fire("click");
        form.reset();
        form.initialize({ ssid: "saved", tcp_cli_enabled: true });
        el("wifi-status").textContent = "Current board";
        el("wifi-dialog").showModal();
        if (succeeds) pending.resolve(operation === "scan" ? [{ ssid: "old", ssid_hex: "6f6c64" }]
          : { ipv4: "192.0.2.1", remaining_seconds: 600 });
        else pending.reject(new Error("old board error"));
        await work;
        assert.equal(el("wifi-status").textContent, "Current board");
        assert.equal(el("wifi-networks").children.length, 0);
        assert.equal(el("wifi-dialog").open, true);
        assert.equal(form.operationBusy(), false);
        assert.equal(form.canSave(), true);
      });
    }
  }
});

test("snapshot initialization preserves drafts and reset admits the next board", async context => {
  const { form, el } = formFixture(context);
  el("wifi-ssid").value = "edited network";
  await el("wifi-ssid").fire("input");
  el("settings-tcp-cli").value = "0";
  await el("settings-tcp-cli").fire("change");
  form.initialize({ ssid: "poll must not overwrite edits", tcp_cli_enabled: true });
  assert.equal(el("wifi-ssid").value, "edited network");
  assert.equal(el("settings-tcp-cli").value, "0");
  assert.equal(form.canSave(), false);
  form.reset();
  form.initialize({ ssid: "current board", tcp_cli_enabled: true });
  assert.equal(el("wifi-ssid").value, "current board");
  assert.equal(el("settings-tcp-cli").value, "1");
  assert.equal(form.canSave(), true);
  await el("wifi-connect").fire("click");
  const staged = form.request();
  form.initialize({ ssid: "poll must not overwrite staged credentials", tcp_cli_enabled: false });
  assert.deepEqual(form.request(), staged);
});

test("Wi-Fi expiry notifies Save eligibility once and stops its countdown", async context => {
  let now = 0, wall = 0, changes = 0;
  const ticks = new Map();
  let timer = 0;
  context.mock.method(Date, "now", () => wall);
  context.mock.method(performance, "now", () => now);
  context.mock.method(globalThis, "setInterval", callback => { ticks.set(++timer, callback); return timer; });
  context.mock.method(globalThis, "clearInterval", id => ticks.delete(id));
  const { form, el } = formFixture(context, {
    changed: () => changes++, test: async () => ({ ipv4: "192.0.2.1", remaining_seconds: 1 }),
  });
  await el("wifi-connect").fire("click");
  assert.equal(form.canSave(), true);
  const before = changes;
  wall = 3600000;
  now = 999;
  for (const tick of ticks.values()) tick();
  assert.equal(form.canSave(), true, "wall-clock changes cannot expire the stage");
  assert.equal(changes, before);
  wall = -3600000;
  now = 1000;
  for (const tick of ticks.values()) tick();
  assert.equal(form.canSave(), false);
  assert.equal(changes, before + 1);
  assert.equal(ticks.size, 0);
  assert.equal(form.canSave(), false, "a backward clock change cannot extend the stage");
});

test("connection-test progress uses monotonic elapsed time despite wall-clock changes", async context => {
  let now = 0, wall = 0;
  const ticks = new Map();
  let timer = 0;
  context.mock.method(performance, "now", () => now);
  context.mock.method(Date, "now", () => wall);
  context.mock.method(globalThis, "setInterval", callback => { ticks.set(++timer, callback); return timer; });
  context.mock.method(globalThis, "clearInterval", id => ticks.delete(id));
  const pending = deferred();
  const { el } = formFixture(context, { test: () => pending.promise });
  const work = el("wifi-connect").fire("click");
  wall = 3600000;
  now = 2000;
  for (const tick of ticks.values()) tick();
  assert.match(el("wifi-status").textContent, /2s \(up to 30s\)/);
  wall = -3600000;
  now = 30000;
  for (const tick of ticks.values()) tick();
  assert.match(el("wifi-status").textContent, /30s; waiting for the final result/);
  pending.resolve({ ipv4: "192.0.2.1", remaining_seconds: 600 });
  await work;
});

test("scanned network buttons expose raw-SSID selection and clear it for manual entry", async context => {
  const { form, el } = formFixture(context, {
    scan: async () => [{ ssid: "one", ssid_hex: "6f6e65" }, { ssid: "two", ssid_hex: "74776f" }],
  });
  await el("wifi-scan").fire("click");
  const [one, two] = el("wifi-networks").children;
  await one.fire("click");
  assert.equal(one.attributes["aria-pressed"], "true");
  assert.equal(two.attributes["aria-pressed"], "false");
  await two.fire("click");
  assert.equal(one.attributes["aria-pressed"], "false");
  assert.equal(two.attributes["aria-pressed"], "true");
  await el("wifi-other").fire("click");
  assert.equal(two.attributes["aria-pressed"], "false");
  assert.equal(el("wifi-ssid").value, "");
  assert.equal(form.canSave(), false);
});

test("keyless Bluetooth reports that a control key is needed; unavailable reports stay distinct", () => {
  const previous = globalThis.document;
  const elements = new Map();
  globalThis.document = { querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, {});
    return elements.get(selector);
  } };
  const snapshot = { connected: true, hardware: { capabilities: 128, serial_number: "AB" }, wireless: {
    wifi_enabled: true, ssid: "saved", wifi_state: "failed", ble_enabled: true, ble_state: "off",
    ble_key_configured: false, restart_required: false,
  } };
  try {
    renderWirelessStatus(snapshot);
    assert.equal(elements.get("#ble-state").textContent, "Off (no control key)");
    assert.doesNotMatch(elements.get("#wifi-state").textContent, /key/);
    renderWirelessStatus({ ...snapshot, wireless: { ...snapshot.wireless, ble_key_configured: true } });
    assert.equal(elements.get("#ble-state").textContent, "PicoForge-AB (starting)");
    renderWirelessStatus({ ...snapshot, connected: false });
    assert.equal(elements.get("#wifi-state").textContent, "--");
    renderWirelessStatus({ ...snapshot, wireless_error: "Read failed" });
    assert.equal(elements.get("#wifi-state").textContent, "Unavailable");
    assert.equal(elements.get("#wifi-state").title, "Read failed");
  } finally { globalThis.document = previous; }
});

test("TCP switch is saved and expired test credentials block saving", async context => {
  const previous = globalThis.document;
  const elements = new Map();
  const el = id => {
    if (!elements.has(id)) elements.set(id, {
      value: "", children: [], handlers: {},
      label: { hidden: true, toggleAttribute(name, hidden) { this[name] = hidden; } },
      closest() { return this.label; },
      options: ["0", "1", "configure"].map(value => ({ value })),
      addEventListener(event, callback) { this.handlers[event] = callback; },
      replaceChildren() {}, close() {}, showModal() {},
    });

    return elements.get(id);
  };
  globalThis.document = { querySelector: selector => el(selector.slice(1)) };
  let now = 100;
  const previousCrypto = Object.getOwnPropertyDescriptor(globalThis, "crypto");
  context.mock.method(performance, "now", () => now);
  let status = { ssid: "saved", wifi_enabled: true, tcp_cli_enabled: true,
    ble_key_configured: false };
  let supported = true, ready = true;
  try {
    el("settings-wifi").value = "1";
    el("settings-ble").value = "0";
    const form = bindWirelessForm({ ready: () => ready, supported: () => supported,
      test: async () => ({ ipv4: "192.168.1.2", remaining_seconds: 600 }), cancelTest: async () => {} });
    form.initialize(status);
    assert.equal(form.dirty(), false);
    assert.equal(el("settings-tcp-cli").disabled, false);
    assert.equal(el("settings-tcp-cli").label.hidden, false);
    el("settings-tcp-cli").value = "0";
    el("settings-tcp-cli").handlers.change();
    form.initialize(status);
    assert.equal(el("settings-tcp-cli").value, "0", "periodic refresh preserves unsaved TCP edits");
    assert.equal(form.dirty(), true);
    assert.equal(form.request().tcp_cli_enabled, false);
    form.finish({ ...status, tcp_cli_enabled: false });
    assert.equal(form.dirty(), false);
    const bleOn = () => el("settings-ble").options.find(option => option.value === "1");
    assert.equal(bleOn().disabled, true, "Bluetooth cannot be enabled without a saved or staged key");
    Object.defineProperty(globalThis, "crypto", { configurable: true, value: { getRandomValues: bytes => bytes.fill(1) } });
    el("ble-generate").handlers.click();
    assert.equal(bleOn().disabled, false);
    el("settings-tcp-cli").value = "1";
    el("settings-tcp-cli").handlers.change();
    assert.equal(el("settings-ble").value, "1", "generating a key enables Bluetooth");
    assert.equal(Object.hasOwn(form.request(), "tcp_cli_auth_required"), false, "TCP auth follows the key");
    el("settings-ble").value = "0";
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, false, "TCP CLI only depends on Wi-Fi");
    ready = false;
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, true);
    ready = true;
    form.update();
    el("wifi-ssid").value = "tested";
    await el("wifi-connect").handlers.click();
    assert.equal(form.request().use_tested_wifi, true);
    assert.equal(form.canSave(), true);
    el("wifi-ssid").value = "edited-network";
    el("wifi-ssid").handlers.input();
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /test the connection again/);
    el("wifi-password").value = "new-passphrase";
    el("wifi-password").handlers.input();
    assert.equal(form.canSave(), false);
    await el("wifi-connect").handlers.click();
    assert.equal(form.request().ssid, "edited-network");
    assert.equal(form.request().use_tested_wifi, true);
    now += 600000;
    form.update();
    assert.equal(form.canSave(), false);
    assert.throws(() => form.request(), /expired/);
    assert.match(el("wireless-summary").textContent, /expired/);
    el("settings-wifi").value = "0";
    form.update();
    assert.equal(el("settings-tcp-cli").disabled, true);
    assert.equal(el("settings-tcp-cli").value, "1");
    assert.equal(form.canSave(), true, "expired credentials do not prevent disabling Wi-Fi");
    const disabled = form.request();
    assert.equal(disabled.wifi_enabled, false);
    assert.equal(disabled.password, "");
    assert.equal(disabled.ssid, "");
    assert.equal(Object.hasOwn(disabled, "use_tested_wifi"), false);
    assert.equal(el("wireless-summary").hidden, true);
    form.reset();
    form.update();
    supported = false;
    form.update();
    for (const id of ["settings-tcp-cli"]) {
      assert.equal(el(id).label.hidden, true, "non-wireless boards hide TCP controls");
      assert.equal(el(id).disabled, true);
    }
  } finally {
    globalThis.document = previous;
    if (previousCrypto) Object.defineProperty(globalThis, "crypto", previousCrypto);
    else delete globalThis.crypto;
  }
});

test("scanned opaque SSIDs retain their bytes through testing and saving; manual edits use UTF-8", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const node = () => ({
    value: "", children: [], handlers: {}, dataset: {}, setAttribute() {},
    options: ["0", "1", "configure"].map(value => ({ value })),
    label: { toggleAttribute() {} }, closest() { return this.label; },
    addEventListener(event, callback) { this.handlers[event] = callback; },
    replaceChildren(...children) { this.children = children; },
    append(...children) { this.children.push(...children); },
    showModal() { this.open = true; }, close() { this.open = false; },
  });
  const el = id => {
    if (!elements.has(id)) elements.set(id, node());
    return elements.get(id);
  };
  globalThis.document = { querySelector: selector => el(selector.slice(1)), createElement: node };
  const requests = [];
  let form;
  try {
    el("settings-wifi").value = "1";
    el("settings-ble").value = "0";
    form = bindWirelessForm({
      ready: () => true, supported: () => true,
      scan: async () => [{ ssid: "\\xFF\\x00", ssid_hex: "ff00", rssi: -30, secured: false }],
      test: async request => { requests.push(request); return { ipv4: "192.0.2.1", remaining_seconds: 600 }; },
      cancelTest: async () => {},
    });
    form.initialize({ ssid: "saved", ssid_hex: "7361766564", tcp_cli_enabled: true });
    await el("wifi-scan").handlers.click();
    el("wifi-networks").children[0].handlers.click();
    await el("wifi-connect").handlers.click();
    assert.equal(requests[0].ssid_hex, "ff00");
    assert.equal(form.request().ssid_hex, "ff00");
    assert.equal(form.request().use_tested_wifi, true);
    el("wifi-ssid").value = "manual";
    el("wifi-ssid").handlers.input();
    await el("wifi-connect").handlers.click();
    assert.equal(Object.hasOwn(requests[1], "ssid_hex"), false);
    assert.equal(Object.hasOwn(form.request(), "ssid_hex"), false);
  } finally {
    form?.reset();
    globalThis.document = previous;
  }
});

test("failed temporary Wi-Fi cancellation retains the stage and permits a safe retry", async () => {
  const previous = globalThis.document;
  const elements = new Map();
  const el = id => {
    if (!elements.has(id)) elements.set(id, {
      value: id === "settings-wifi" ? "1" : "", textContent: "", children: [], handlers: {},
      options: ["0", "1", "configure"].map(value => ({ value })),
      label: { hidden: false, toggleAttribute(name, hidden) { this[name] = hidden; } },
      closest() { return this.label; },
      addEventListener(event, callback) { this.handlers[event] = callback; },
      replaceChildren(...children) { this.children = children; },
      append(...children) { this.children.push(...children); },
      showModal() { this.open = true; },
      close() { this.open = false; },
    });
    return elements.get(id);
  };
  globalThis.document = {
    querySelector: selector => el(selector.slice(1)),
    createElement: () => ({ children: [], append(...children) { this.children.push(...children); } }),
  };
  let cancelCalls = 0;
  let cancelFailure = "busy";
  let testFailure = false;
  try {
    const form = bindWirelessForm({
      scan: async () => [],
      test: async () => {
        if (testFailure) throw new Error("test could not connect");
        return { ipv4: "192.168.1.2", remaining_seconds: 600 };
      },
      cancelTest: async () => {
        cancelCalls++;
        if (cancelFailure === "busy") throw new Error("device is busy");
        if (cancelFailure === "timeout") throw new DOMException("request timed out", "TimeoutError");
      },
      ready: () => true,
      supported: () => true,
    });
    form.initialize({ ssid: "saved", wifi_enabled: true, ble_key_configured: false });
    el("wifi-ssid").value = "tested-network";
    await el("wifi-connect").handlers.click();
    el("wifi-dialog").showModal();
    await el("wifi-cancel").handlers.click();

    assert.equal(cancelCalls, 1);
    assert.equal(el("wifi-dialog").open, true, "keep the dialog open when cleanup fails");
    assert.equal(el("wifi-cancel").textContent, "Retry clearing test");
    assert.match(el("wifi-status").textContent, /device is busy/);
    assert.match(el("wireless-summary").textContent, /Open Wi-Fi settings to retry/);
    assert.equal(form.stageValid(), false);
    assert.equal(form.canSave(), false);
    assert.equal(form.operationBusy(), true, "board selection stays locked until temporary credentials are cleared");
    assert.equal(el("wifi-connect").disabled, true, "block another wireless operation until cleanup succeeds");
    assert.throws(() => form.request(), /Clear the temporary Wi-Fi test/);

    cancelFailure = "";
    await el("wifi-cancel").handlers.click();
    assert.equal(cancelCalls, 2);
    assert.equal(el("wifi-dialog").open, false);
    assert.equal(form.stageValid(), true);
    assert.equal(Object.hasOwn(form.request(), "use_tested_wifi"), false);
    assert.equal(el("wireless-summary").hidden, true);

    testFailure = true;
    el("wifi-ssid").value = "failed-network";
    await el("wifi-connect").handlers.click();
    el("wifi-dialog").showModal();
    await el("wifi-cancel").handlers.click();
    assert.equal(cancelCalls, 3, "failed tests still leave candidates that require explicit discard");
    assert.equal(el("wifi-dialog").open, false);

    testFailure = false;
    el("wifi-ssid").value = "second-network";
    await el("wifi-connect").handlers.click();
    el("wifi-dialog").showModal();
    cancelFailure = "timeout";
    await el("wifi-cancel").handlers.click();
    assert.equal(form.operationBusy(), true, "unknown cleanup outcomes keep board operations locked");
    assert.match(el("wireless-summary").textContent, /Use Re-sync.*reconnect the board and reload the dashboard/);
    assert.equal(el("wifi-dialog").open, true);
    form.reset();
  } finally {
    globalThis.document = previous;
  }
});

test("Wi-Fi label links only with a connected board, a started listener and a local console URL", () => {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      children: [],
      set textContent(value) { this.text = value; this.children = []; },
      get textContent() { return this.text; },
      querySelector() { return this.children[0]; },
      replaceChildren(...children) { this.children = children; },
    });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({}) };
  try {
    const state = { connected: true, hardware: { capabilities: 128 }, console_url: "/console?board=board-a",
      wireless: { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true,
        ssid: "HomeDev", ipv4: "192.168.66.87" } };
    renderWirelessStatus(state);
    const link = element("#wifi-state").children[0];
    assert.equal(link.textContent, "HomeDev (192.168.66.87)");
    assert.equal(link.href, "/console?board=board-a");
    assert.equal(link.target, "_blank");
    assert.equal(link.rel, "noopener noreferrer");
    renderWirelessStatus(state);
    assert.equal(element("#wifi-state").children[0], link, "polling preserves the focused link");
    for (const change of [{ connected: false }, { wireless_error: "read failed" }, { console_url: undefined },
      { wireless: { ...state.wireless, tcp_cli_started: false } },
      { wireless: { ...state.wireless, wifi_state: "connecting" } }]) {
      renderWirelessStatus({ ...state, ...change });
      assert.equal(element("#wifi-state").children.length, 0);
    }
  } finally { globalThis.document = saved; }
});
