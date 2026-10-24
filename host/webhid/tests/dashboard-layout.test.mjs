import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { PicoForgeWebHid } from "../js/device.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../js/device.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

test("standalone dashboard labels profiles, active measurements, controls and accessible statuses", () => {
  const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>PicoForge Diagnostics \| WebHID<\/title>/);
  assert.match(html, /<label for="settings-storage">SD Card/);
  assert.match(html, /value="0">Standard/);
  assert.match(html, /value="1">High-Speed \(requested\)/);
  assert.match(html, /id="settings-save"[^>]*>Save settings/);
  assert.match(html, /id="settings-reboot"[^>]*>Reboot/);
  assert.doesNotMatch(html, /Save &amp; Reboot|25 MHz|50 MHz|Core 1 \/ Core 2/);
  assert.match(html, /Firmware RAM reserved \/ total/);
  assert.match(html, /Service load \(Core 0 \/ Core 1\)/);
  assert.match(html, /Last mounted filesystem/);
  assert.match(html, /id="memory-timing"/);
  assert.match(html, /id="led-control"[\s\S]*?aria-pressed="false"/);
  assert.match(html, /<progress[^>]*aria-label="Self-test progress"/);
  for (const name of ["telemetry", "storage", "self-test"]) {
    assert.match(html, new RegExp(`id="${name}-freshness" role="status"`));
  }
});

test("WebHID UI preserves unsaved profiles, separates confirmed reboot, and confirms force return", async () => {
  const saved = {
    document: globalThis.document, window: globalThis.window,
    navigator: Object.getOwnPropertyDescriptor(globalThis, "navigator"),
    secure: Object.getOwnPropertyDescriptor(globalThis, "isSecureContext"),
  };
  const prototypes = new Map(["connect", "setMsc", "saveSettings", "reboot"].map(name =>
    [name, PicoForgeWebHid.prototype[name]]));
  const elements = new Map();
  const el = selector => {
    if (!elements.has(selector)) elements.set(selector, { value: "0", dataset: {}, valueAsNumber: 32,
      addEventListener(type, handler) { this[type] = handler; }, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  let client;
  let confirmed = false;
  let warning = "";
  const commands = [];
  globalThis.document = { querySelector: el, createElement: () => ({}) };
  globalThis.window = { confirm(message) { warning = message; return confirmed; } };
  Object.defineProperty(globalThis, "navigator", { configurable: true, value: { hid: new EventTarget() } });
  Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
  PicoForgeWebHid.prototype.connect = async function () {
    client = this;
    const hardware = decodeIdentityReport(identity());
    this.update({ connected: true, hardware, settings: decodeConfigReport(config(), hardware),
      settings_supported: true, freshness: { 3: "fresh", 10: "fresh", 11: "fresh" },
      msc: { enabled: false, transition_pending: false }, self_test: decodeSelfTestReport(selfTest()),
      memory: { sample_valid: true, initialized: true } });
  };
  PicoForgeWebHid.prototype.setMsc = async (...args) => { commands.push(["msc", ...args]); };
  PicoForgeWebHid.prototype.saveSettings = async function (settings) {
    commands.push(["save", settings]);
    this.update({ settings: { ...this.state.settings, ...settings } });
    return true;
  };
  PicoForgeWebHid.prototype.reboot = async confirmed => { commands.push(["reboot", confirmed]); };
  try {
    await import(`../js/app.mjs?test=${Date.now()}`);
    await el("#connection-control").click();
    assert.equal(el("#settings-storage").value, "1");
    el("#settings-clock").value = "150"; el("#settings-clock").change();
    client.update({ error: null });
    assert.equal(el("#settings-clock").value, "150");
    await el("#settings-save").click();
    assert.deepEqual(commands, [["save", { system_clock_mhz: 150, sdcard_profile: 1 }]]);
    await el("#settings-reboot").click();
    assert.equal(commands.length, 1);
    confirmed = true; await el("#settings-reboot").click();
    assert.deepEqual(commands[1], ["reboot", true]);
    await el("#msc-control").click();
    assert.deepEqual(commands[2], ["msc", true, false]);
    client.update({ msc: { enabled: true, transition_pending: false } });
    confirmed = false; await el("#msc-control").click();
    assert.equal(commands.length, 3);
    assert.match(warning, /writes may be lost.*filesystem may be corrupted/);
    confirmed = true; await el("#msc-control").click();
    assert.deepEqual(commands[3], ["msc", false, true]);
    client.update({ outcome_uncertain: true });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    client.update({ outcome_uncertain: false, freshness: { 3: "stale", 10: "stale", 11: "stale" },
      runtime: null, board_status: null, msc: null, self_test: null });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    assert.equal(el("#self-test-start").disabled, true);
  } finally {
    for (const [name, method] of prototypes) PicoForgeWebHid.prototype[name] = method;
    globalThis.document = saved.document; globalThis.window = saved.window;
    if (saved.navigator) Object.defineProperty(globalThis, "navigator", saved.navigator);
    else delete globalThis.navigator;
    if (saved.secure) Object.defineProperty(globalThis, "isSecureContext", saved.secure);
    else delete globalThis.isSecureContext;
  }
});
