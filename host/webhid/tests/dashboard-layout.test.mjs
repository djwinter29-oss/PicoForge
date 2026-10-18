import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

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
    assert.match(liveReport, /SD card presence/);
    assert.match(liveReport, /id="card-presence"/);
    assert.match(liveReport, /<dt>Memory type<\/dt><dd id="memory-type">/);
    assert.doesNotMatch(liveReport, /USB device|id="usb-state"/);
    const memoryReport = section('aria-labelledby="memory-title"');
    assert.doesNotMatch(memoryReport, /id="memory-presence"|<dt>Presence<\/dt>/);
    assert.doesNotMatch(memoryReport, /id="memory-type"/);
    assert.equal((html.match(/id="memory-type"/g) ?? []).length, 1);
    assert.match(memoryReport, /<dt>Manufacturer<\/dt>/);
    const selfTest = section('aria-labelledby="self-test-title"');
    assert.match(selfTest, /<h2 id="self-test-title">Memory Tests<\/h2>/);
    assert.match(selfTest, /id="msc-control"/);
    assert.match(selfTest, /id="msc-note"/);
    assert.match(selfTest, /id="msc-force-return"/);
    assert.match(selfTest, /aria-describedby="msc-force-warning"/);
    assert.match(selfTest, /lose cached host writes and corrupt the filesystem/);
    assert.match(selfTest, /id="self-test-start"/);
    if (name === "webhid") {
      assert.match(html, /id="connect"/);
      assert.match(html, /id="disconnect"/);
    }

  });
}

test("shared dashboard sends no force request until the user confirms the data-loss warning", async () => {
  const previousDocument = globalThis.document;
  const previousWindow = globalThis.window;
  const previousFetch = globalThis.fetch;
  const elements = new Map();
  const requests = [];
  let confirmed = false;
  let warning = "";
  globalThis.document = { querySelector(selector) {
    if (!elements.has(selector)) elements.set(selector, {
      dataset: {}, valueAsNumber: 32, addEventListener(event, handler) { this[event] = handler; },
    });
    return elements.get(selector);
  } };
  globalThis.window = {
    setInterval() {},
    confirm(message) { warning = message; return confirmed; },
  };
  globalThis.fetch = async (url, options) => {
    if (options?.method === "POST") requests.push([url, JSON.parse(options.body)]);
    return { ok: true, json: async () => url === "/api/status"
      ? { connected: true, msc: { enabled: true, force_return_supported: true } }
      : { msc: { enabled: true, force_return_supported: true, transition_pending: true } } };
  };
  try {
    await import("../../web/js/app.mjs");
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(elements.get("#msc-force-return").disabled, false);
    await elements.get("#msc-force-return").click();
    assert.match(warning, /writes may be lost.*filesystem may be corrupted/);
    assert.equal(requests.length, 0);
    confirmed = true;
    await elements.get("#msc-force-return").click();
    assert.deepEqual(requests, [["/api/msc/force-return", { confirm_data_loss: true }]]);
    assert.equal(elements.get("#msc-force-return").disabled, true);
  } finally {
    globalThis.document = previousDocument;
    globalThis.window = previousWindow;
    globalThis.fetch = previousFetch;
  }
});
