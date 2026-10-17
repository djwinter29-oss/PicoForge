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
    assert.match(selfTest, /id="self-test-start"/);
    if (name === "webhid") {
      assert.match(html, /id="connect"/);
      assert.match(html, /id="disconnect"/);
    }
  });
}
