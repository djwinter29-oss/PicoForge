import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

test("standalone dashboard labels profiles, active measurements, controls and accessible statuses", () => {
  const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>PicoForge Dashboard \| Direct<\/title>/);
  assert.match(html, /rel="icon"[^>]*href="favicon\.svg\?v=2"/);
  assert.match(html, /href="css\/direct\.css\?v=16"/);
  assert.match(html, /src="js\/app\.mjs\?v=33"/);
  const app = readFileSync(new URL("../js/app.mjs", import.meta.url), "utf8");
  assert.match(app, /from "\.\/device\.mjs\?v=7"/);
  assert.match(app, /from "\.\/view\.mjs\?v=14"/);
  assert.doesNotMatch(html, /id="connection-control"|id="connection-transport"/);
  for (const name of ["usb", "bluetooth"]) {
    assert.match(html, new RegExp(`id="${name}-open"[\\s\\S]*?aria-haspopup="dialog"`));
    assert.match(html, new RegExp(`<dialog[^>]*id="${name}-connection-dialog"[\\s\\S]*?aria-labelledby=`));
    assert.match(html, new RegExp(`id="${name}-connection-status" role="status" aria-live="polite"`));
  }
  assert.match(html, /id="bluetooth-access-step" hidden/);
  assert.match(html, /id="ble-view-only"[^>]*>Continue view only<\/button>/);
  assert.match(html, /id="ble-access-status" role="status" aria-live="polite" hidden/);
  assert.match(html, /<label for="settings-storage">SD Card/);
  assert.match(html, /value="0">Standard/);
  assert.match(html, /value="1">High-Speed/);
  assert.match(html, /id="settings-save"[^>]*>Save&amp;Reboot/);
  assert.match(html, /id="settings-reboot"[^>]*>Reboot/);
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
      ["settings-clock", "settings-storage", "settings-wifi", "settings-ble", "settings-tcp-cli", "settings-tcp-auth"]);
    assert.match(board, /<label for="settings-wifi" hidden>[\s\S]*<label for="settings-ble" hidden>/);
    for (const id of ["settings-tcp-cli", "settings-tcp-auth"]) {
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
  assert.doesNotMatch(html, /25 MHz|50 MHz|Core 1 \/ Core 2/);
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
    ["Uptime", "Reset cause", "Service load (Core 0 / Core 1)", "Board temperature", "CLI port", "Storage bus speed",
      "Wi-Fi", "Bluetooth"]);
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
