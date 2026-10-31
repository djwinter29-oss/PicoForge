import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { PicoForgeWebHid } from "../js/device.mjs?v=4";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../js/device.mjs";
import { identity, config, selfTest } from "./support/fixtures.mjs";

test("standalone dashboard labels profiles, active measurements, controls and accessible statuses", () => {
  const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>PicoForge Dashboard \| WebHID<\/title>/);
  assert.match(html, /rel="icon"[^>]*href="favicon\.svg\?v=2"/);
  assert.match(html, /href="css\/webhid\.css\?v=10"/);
  assert.match(html, /src="js\/app\.mjs\?v=15"/);
  const app = readFileSync(new URL("../js/app.mjs", import.meta.url), "utf8");
  assert.match(app, /from "\.\/device\.mjs\?v=4"/);
  assert.match(app, /from "\.\/view\.mjs\?v=6"/);
  assert.match(html, /<label for="settings-storage">SD Card/);
  assert.match(html, /value="0">Standard/);
  assert.match(html, /value="1">High-Speed/);
  assert.match(html, /id="settings-save"[^>]*>Save and Reboot/);
  assert.match(html, /id="settings-reboot"[^>]*>Reboot/);
  for (const path of ["../index.html", "../../web/index.html"]) {
    const dashboard = readFileSync(new URL(path, import.meta.url), "utf8");
    const settings = dashboard.split('aria-labelledby="settings-title"')[1].split("</section>")[0];
    const footer = dashboard.split('<footer class="status-line"')[1].split("</footer>")[0];
    assert.doesNotMatch(settings, /id="settings-note"/);
    assert.match(settings, /id="wireless-actions" hidden/);
    assert.match(settings, /id="wifi-open"[^>]*>Configure Wi-Fi/);
    assert.match(settings, /id="ble-open"[^>]*>Configure Bluetooth/);
    const board = settings.split('<div class="settings-controls">')[1].split("</div>")[0];
    assert.deepEqual([...board.matchAll(/<label for="([^"]+)"/g)].map(match => match[1]),
      ["settings-clock", "settings-storage", "settings-wifi", "settings-ble"]);
    assert.match(board, /<label for="settings-wifi" hidden>[\s\S]*<label for="settings-ble" hidden>/);
    assert.doesNotMatch(settings, /id="wifi-ssid"|id="wifi-password"|id="ble-secret"/);
    assert.match(dashboard, /<dialog[^>]*id="wifi-dialog"/);
    assert.match(dashboard, /<dialog[^>]*id="ble-dialog"/);
    assert.doesNotMatch(dashboard, /id="wireless-save"/);
    assert.match(dashboard, /id="configuration-wireless-warning" hidden/);
    assert.match(footer, /role="status" aria-live="polite"/);
    assert.match(footer, /id="status-message"[\s\S]*id="settings-note"/);
    assert.equal([...dashboard.matchAll(/id="settings-note"/g)].length, 1);
    assert.match(dashboard, /id="self-test-start"[^>]*>Start benchmark<\/button>/);
    assert.doesNotMatch(dashboard, />Start self-test<\/button>/);
    assert.match(dashboard, /<details class="pin-map-disclosure self-test-disclosure" id="self-test-disclosure">/);
    assert.match(dashboard, /<summary>\s*<h2[^>]*>Storage benchmark<\/h2>\s*<span[^>]*id="self-test-state"/);
    assert.match(dashboard, /<\/button>\s*<\/div>\s*<progress id="self-test-progress"/);
    const benchmark = dashboard.split('aria-labelledby="self-test-title"')[1].split("</section>")[0];
    assert.deepEqual([...benchmark.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
      ["Written / target", "Write duration", "Average write speed", "Verified / target", "Read duration",
        "Average read speed"]);
  }
  assert.doesNotMatch(html, /Save &amp; Reboot|25 MHz|50 MHz|Core 1 \/ Core 2/);
  assert.match(html, /Firmware RAM reserved \/ total/);
  assert.match(html, /Service load \(Core 0 \/ Core 1\)/);
  assert.match(html, /<dt>Filesystem<\/dt>/);
  assert.doesNotMatch(html, /class="eyebrow"|id="mcu"|id="memory-timing"|id="board-config"/);
  assert.doesNotMatch(html, /metric-product/);
  assert.match(html, /id="settings-title">Board configuration/);
  const identity = html.split('aria-label="Hardware identity"')[1].split("</section>")[0];
  assert.deepEqual([...identity.matchAll(/<strong id="([^"]+)"/g)].map(match => match[1]),
    ["product-name", "board-model", "serial", "identity-clock", "ram-usage", "firmware-version"]);
  const runtime = html.split('<dl class="runtime-grid runtime-summary">')[1].split("</dl>")[0];
  assert.deepEqual([...runtime.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Uptime", "Reset cause", "Service load (Core 0 / Core 1)", "Board temperature", "CLI port", "Storage bus speed"]);
  const storage = html.split('aria-labelledby="memory-title"')[1].split("</section>")[0];
  assert.deepEqual([...storage.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
    ["Memory state", "Capacity", "Filesystem", "Manufacturer", "Product", "Serial"]);
  assert.doesNotMatch(html, /id="memory-mode"/);
  assert.match(html, /<details class="pin-map-disclosure">/);
  assert.match(html, /id="board-uart"[\s\S]*id="board-storage-pins"/);
  for (const name of ["configuration", "reboot", "return", "share"]) {
    assert.match(html, new RegExp(`<dialog class="confirmation-dialog" id="${name}-dialog"`));
  }
  assert.match(html, /id="led-control"[\s\S]*?aria-pressed="false"/);
  assert.match(html, /<progress[^>]*aria-label="Benchmark progress"/);
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
      open: false, showModal() { this.open = true; }, close() { this.open = false; },
      addEventListener(type, handler) { this[type] = handler; }, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; } });
    return elements.get(selector);
  };
  let client;
  const commands = [];
  globalThis.document = { querySelector: el, createElement: () => ({ children: [],
    append(...children) { this.children.push(...children); } }) };
  globalThis.window = { confirm() { assert.fail("Use styled dialogs, not browser confirmations"); } };
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
  PicoForgeWebHid.prototype.saveSettings = async function (settings, force = false) {
    commands.push(["save", settings]);
    this.testForceSave = force;
    this.update({ settings: { ...this.state.settings, ...settings } });
    return true;
  };
  PicoForgeWebHid.prototype.reboot = async confirmed => { commands.push(["reboot", confirmed]); };
  try {
    await import(`../js/app.mjs?test=${Date.now()}`);
    await el("#connection-control").click();
    assert.equal(el("#settings-storage").value, "1");
    assert.equal(el("#settings-note").hidden, true);
    assert.equal(el("#status-message").hidden, false);
    assert.equal(el("#settings-save").disabled, true);
    client.update({ self_test_command_pending: true });
    assert.equal(el("#settings-note").hidden, true);
    client.update({ self_test_command_pending: false, self_test: decodeSelfTestReport(selfTest(1)) });
    assert.equal(el("#settings-note").hidden, true);
    client.update({ self_test: decodeSelfTestReport(selfTest()) });
    client.update({ msc_command_pending: true });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, true);
    client.update({ msc_command_pending: false });
    assert.equal(el('[aria-labelledby="self-test-title"]').hidden, false);
    el("#settings-clock").value = "250"; el("#settings-clock").change();
    client.update({ error: null });
    assert.equal(el("#settings-clock").value, "250");
    await el("#settings-save").click();
    assert.deepEqual(commands, []);
    assert.equal(el("#configuration-dialog").open, true);
    assert.match(el("#configuration-dialog-profile").textContent, /250 MHz.*High-Speed/);
    el("#cancel-configuration").click();
    await el("#confirm-configuration").click();
    assert.deepEqual(commands, []);
    await el("#settings-save").click();
    await el("#confirm-configuration").click();
    assert.deepEqual(commands, [["save", { system_clock_mhz: 250, sdcard_profile: 1, radios: 0 }]]);
    await el("#settings-reboot").click();
    assert.equal(commands.length, 1);
    assert.equal(el("#reboot-dialog").open, true);
    el("#reboot-dialog").keydown({ key: "Escape", preventDefault() {} });
    assert.equal(el("#reboot-dialog").open, false);
    await el("#confirm-reboot").click();
    assert.equal(commands.length, 1);
    await el("#settings-reboot").click();
    await el("#confirm-reboot").click();
    assert.deepEqual(commands[1], ["reboot", true]);
    await el("#msc-control").click();
    assert.equal(el("#share-dialog").open, true);
    assert.equal(commands.length, 2);
    el("#cancel-share").click();
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    await el("#msc-control").click();
    el("#share-dialog").keydown({ key: "Escape", preventDefault() {} });
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    await el("#msc-control").click();
    client.update({ self_test_command_pending: true });
    await el("#confirm-share").click();
    assert.equal(commands.length, 2);
    client.update({ self_test_command_pending: false });
    await el("#msc-control").click();
    await el("#confirm-share").click();
    assert.deepEqual(commands[2], ["msc", true, false]);
    client.update({ msc: { enabled: true, transition_pending: false } });
    el("#settings-clock").value = "200"; el("#settings-clock").change();
    assert.equal(el("#settings-clock").disabled, false);
    assert.equal(el("#settings-save").disabled, true);
    client.update({ hardware: { ...client.state.hardware, capabilities: client.state.hardware.capabilities | 64 } });
    await el("#settings-save").click();
    assert.equal(el("#configuration-force-warning").hidden, false);
    el("#cancel-configuration").click();
    await el("#confirm-configuration").click();
    assert.equal(commands.length, 3);
    await el("#settings-save").click();
    await el("#confirm-configuration").click();
    assert.equal(client.testForceSave, true);
    commands.pop();
    await el("#msc-control").click();
    assert.equal(commands.length, 3);
    assert.equal(el("#return-dialog").open, true);
    el("#cancel-return").click();
    await el("#confirm-return").click();
    assert.equal(commands.length, 3);
    await el("#msc-control").click();
    await el("#confirm-return").click();
    assert.deepEqual(commands[3], ["msc", false, true]);
    await el("#msc-control").click();
    client.update({ outcome_uncertain: true });
    assert.equal(el("#msc-control").disabled, true);
    assert.equal(el("#settings-save").disabled, true);
    await el("#confirm-return").click();
    assert.equal(commands.length, 4);
    client.update({ outcome_uncertain: false });
    await el("#msc-control").click();
    assert.equal(el("#return-dialog").open, true);
    client.update({ connected: false });
    assert.equal(el("#return-dialog").open, false);
    await el("#confirm-return").click();
    assert.equal(commands.length, 4);
    client.update({ connected: true });
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
