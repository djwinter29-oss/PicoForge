import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { PicoForgeWebHid } from "../js/device.mjs";

for (const name of ["web", "webhid"]) {
  test(`${name} places controls beside their reports without refresh or write-consent inputs`, () => {
    const html = readFileSync(new URL(`../../${name}/index.html`, import.meta.url), "utf8");
    const section = label => {
      const markup = html.match(new RegExp(`<section[^>]*${label}[^>]*>[\\s\\S]*?</section>`));
      assert.ok(markup, `${label} section must exist`);
      return markup[0];
    };
    assert.equal((html.match(/<main>/g) ?? []).length, 1);
    assert.equal((html.match(/<\/main>/g) ?? []).length, 1);
    assert.doesNotMatch(html, /id="refresh"|self-test-consent|type="checkbox"/);
    assert.match(section('class="identity"'), /id="firmware-version"/);
    const liveReport = section('aria-labelledby="runtime-title"');
    assert.match(liveReport, /id="led-control"/);
    assert.match(liveReport, /id="cpu-usage"/);
    assert.match(liveReport, /id="ram-usage"/);
    assert.doesNotMatch(liveReport, /System clock|runtime-clock/);
    assert.doesNotMatch(html, /Report sequence|id="sequence"/);
    assert.equal((html.match(/<span>System clock<\/span>/g) ?? []).length, 1);
    assert.match(liveReport, /SD presence/);
    assert.match(liveReport, /id="card-presence"/);
    assert.doesNotMatch(html, /Memory type|Memory presence|id="memory-type"/);
    assert.doesNotMatch(liveReport, /USB device|id="usb-state"/);
    const memoryReport = section('aria-labelledby="memory-title"');
    assert.doesNotMatch(memoryReport, /id="memory-presence"|<dt>Presence<\/dt>/);
    assert.doesNotMatch(memoryReport, /id="memory-type"/);
    assert.match(memoryReport, /<dt>Manufacturer<\/dt>/);
    const selfTest = section('aria-labelledby="self-test-title"');
    assert.match(selfTest, /<h2 id="self-test-title">Memory Tests<\/h2>/);
    assert.match(selfTest, /id="msc-control"/);
    assert.match(selfTest, /id="msc-note"/);
    assert.doesNotMatch(selfTest, /msc-force-return|Force return to Pico/);
    assert.doesNotMatch(selfTest, /msc-warning|Firmware owns storage|Disabling can lose/);
    assert.match(selfTest, /id="msc-note" role="status" hidden><\/p>/);
    assert.match(selfTest, /Writes and verifies \/PFTEST.BIN, replacing any existing file at that path/);
    assert.match(selfTest, /id="self-test-start"/);
    assert.match(selfTest, /id="self-test-size"[^>]*max="1024"/);
    if (name === "webhid") {
      assert.match(html, /id="connect"/);
      assert.match(html, /id="disconnect"/);
      const settings = section('aria-labelledby="settings-title"');
      assert.match(settings, /id="settings-clock"/);
      assert.match(settings, /id="settings-sdio"/);
      assert.match(settings, /id="settings-spi"/);
      assert.match(settings, /id="settings-save"/);
    }
  });

}

test("WebHID Disable confirms before sending Pico-side detach and does not send on cancellation", async () => {
    const previousDocument = globalThis.document;
    const previousWindow = globalThis.window;
    const navigatorDescriptor = Object.getOwnPropertyDescriptor(globalThis, "navigator");
    const secureDescriptor = Object.getOwnPropertyDescriptor(globalThis, "isSecureContext");
    const previousConnect = PicoForgeWebHid.prototype.connect;
    const elements = new Map();
    const sent = [];
    let confirmed = false;
    let connectedClient;
    globalThis.document = { querySelector(selector) {
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, valueAsNumber: 32, addEventListener(event, handler) { this[event] = handler; },
      });
      return elements.get(selector);
    } };
    globalThis.window = { confirm: () => confirmed };
    Object.defineProperty(globalThis, "navigator", { configurable: true, value: { hid: new EventTarget() } });
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
    PicoForgeWebHid.prototype.connect = async function () {
      connectedClient = this;
      let settingsReport = Uint8Array.from([1, 200, 0, 0, 50]);
      this.device = {
        sendFeatureReport: async (id, payload) => {
          sent.push([id, ...payload]);
          if (id === 8) settingsReport = Uint8Array.from(payload);
        },
        receiveFeatureReport: async () => new DataView(settingsReport.buffer),
      };
      this.update({ connected: true, settings_supported: true,
        settings: { system_clock_mhz: 200, sdio_bus_mhz: 0, spi_bus_mhz: 50 },
        msc: { enabled: true, force_return_supported: true } });
    };
    try {
      await import("../js/app.mjs");
      await elements.get("#connect").click();
      assert.equal(elements.get("#msc-note").hidden, true);
      assert.equal(elements.get("#settings-clock").value, "200");
      assert.equal(elements.get("#settings-save").disabled, true);
      await elements.get("#msc-control").click();
      assert.equal(sent.length, 0);
      confirmed = true;
      await elements.get("#msc-control").click();
      assert.deepEqual(sent, [[5, 3, 2]]);
      assert.equal(elements.get("#msc-control").disabled, true);
      connectedClient.update({ msc: { enabled: false } });
      confirmed = false;
      await elements.get("#msc-control").click();
      assert.deepEqual(sent[1], [5, 2, 1]);
      connectedClient.update({ msc: { enabled: true, force_return_supported: false } });
      assert.equal(elements.get("#msc-control").disabled, true);
      await elements.get("#msc-control").click();
      assert.equal(sent.length, 2);
      connectedClient.update({ msc: { enabled: false, transition_pending: false } });
      elements.get("#settings-clock").value = "150";
      elements.get("#settings-sdio").value = "0";
      elements.get("#settings-spi").value = "25";
      elements.get("#settings-clock").change();
      await elements.get("#settings-save").click();
      assert.deepEqual(sent.at(-1), [8, 1, 150, 0, 0, 25]);
      assert.match(elements.get("#settings-note").textContent, /after reboot/);
    } finally {
      PicoForgeWebHid.prototype.connect = previousConnect;
      globalThis.document = previousDocument;
      globalThis.window = previousWindow;
      if (navigatorDescriptor) Object.defineProperty(globalThis, "navigator", navigatorDescriptor);
      else delete globalThis.navigator;
      if (secureDescriptor) Object.defineProperty(globalThis, "isSecureContext", secureDescriptor);
      else delete globalThis.isSecureContext;
    }
});

test("shared Disable confirms Pico-side return while Enable needs no confirmation or host eject", async () => {
  const previousDocument = globalThis.document;
  const previousWindow = globalThis.window;
  const previousFetch = globalThis.fetch;
  const elements = new Map();
  const requests = [];
  let confirmed = false;
  let warning = "";
  let poll;
  let enabled = true;
  let supported = true;
  globalThis.document = { querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, {
      dataset: {}, valueAsNumber: 32, addEventListener(event, handler) { this[event] = handler; },
    });
    return elements.get(selector);
  } };
  globalThis.window = {
    setInterval(callback) { poll = callback; },
    confirm(message) { warning = message; return confirmed; },
  };
  globalThis.fetch = async (url, options) => {
    if (options?.method === "POST") requests.push([url, JSON.parse(options.body)]);
    const msc = { enabled, force_return_supported: supported, transition_pending: url !== "/api/status" };
    return { ok: true, json: async () => url === "/api/status"
      ? { connected: true, msc }
      : url === "/api/msc" ? msc : { msc } };
  };
  try {
    await import("../../web/js/app.mjs");
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(elements.get("#msc-control").disabled, false);
    assert.equal(elements.get("#msc-note").hidden, true);
    await elements.get("#msc-control").click();
    assert.match(warning, /writes may be lost.*filesystem may be corrupted/);
    assert.equal(requests.length, 0);
    confirmed = true;
    await elements.get("#msc-control").click();
    assert.deepEqual(requests, [["/api/msc/force-return", { confirm_data_loss: true }]]);
    assert.equal(elements.get("#msc-control").disabled, true);
    enabled = false;
    await poll();
    confirmed = false;
    await elements.get("#msc-control").click();
    assert.deepEqual(requests[1], ["/api/msc", { enabled: true }]);
    assert.equal(elements.get(".status-line").dataset.error, "false");
    enabled = true;
    supported = false;
    await poll();
    assert.equal(elements.get("#msc-control").disabled, true);
    assert.match(elements.get("#msc-note").textContent, /Update firmware/);
    assert.equal(elements.get("#msc-note").hidden, false);
    await elements.get("#msc-control").click();
    assert.equal(requests.length, 2);
  } finally {
    globalThis.document = previousDocument;
    globalThis.window = previousWindow;
    globalThis.fetch = previousFetch;
  }
});
