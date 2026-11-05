import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

test("server dashboard keeps radio dialogs, confirmation controls and benchmark layout", () => {
  for (const path of ["../index.html"]) {
    const dashboard = readFileSync(new URL(path, import.meta.url), "utf8");
    const settings = dashboard.split('aria-labelledby="settings-title"')[1].split("</section>")[0];
    const footer = dashboard.split('<footer class="status-line"')[1].split("</footer>")[0];
    assert.doesNotMatch(settings, /id="settings-note"/);
    assert.doesNotMatch(settings, /wireless-actions|wifi-open|ble-open/);
    assert.match(settings, /id="settings-wifi"[\s\S]*?<option value="configure">Configure…<\/option>/);
    assert.match(settings, /id="settings-ble"[\s\S]*?<option value="configure">Configure…<\/option>/);
    const board = settings.split('<div class="settings-controls">')[1].split("</div>")[0];
    assert.deepEqual([...board.matchAll(/<label for="([^"]+)"/g)].map(match => match[1]),
      ["settings-clock", "settings-storage", "settings-wifi", "settings-ble", "settings-tcp-cli"]);
    assert.match(board, /<label for="settings-wifi" hidden>[\s\S]*<label for="settings-ble" hidden>/);
    for (const id of ["settings-tcp-cli"]) {
      assert.match(board, new RegExp(`<label for="${id}" hidden>[\\s\\S]*?<select id="${id}"`));
      const attributes = board.match(new RegExp(`<select id="${id}"([^>]*)>`))[1];
      assert.match(attributes, /title="[^"]+"/);
      assert.match(attributes, new RegExp(`aria-describedby="${id}-help"`));
      assert.match(board, new RegExp(`<small id="${id}-help" hidden>`));
    }
    assert.doesNotMatch(board, /type="checkbox"/);
    assert.doesNotMatch(settings, /id="wifi-ssid"|id="wifi-password"|id="ble-secret"/);
    assert.match(dashboard, /<dialog[^>]*id="wifi-dialog"/);
    assert.match(dashboard, /<dialog[^>]*id="ble-dialog"/);
    assert.match(dashboard, /id="msc-status-refresh"[^>]*hidden/);
    assert.match(dashboard, /id="wifi-networks"[^>]*role="group"/);
    assert.doesNotMatch(dashboard, /role="listbox"|role="option"/);
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
});
