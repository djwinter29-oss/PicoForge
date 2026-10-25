import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { renderStatus } from "../../web/js/dashboard.mjs";

test("shared dashboard handlers survive a status failure that clears the snapshot mid-command", async () => {
  const handlers = new Map();
  let tick;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) {
      const target = { textContent: "", valueAsNumber: 32, dataset: {}, style: {}, children: [],
        open: false, showModal() { this.open = true; }, close() { this.open = false; },
        replaceChildren(...children) { this.children = children; }, append(child) { this.children.push(child); },
        classList: { toggle() {}, add() {}, remove() {} },
        addEventListener: (type, handler) => handlers.set(`${selector}:${type}`, handler) };
      elements.set(selector, new Proxy(target, { get: (object, key) => key in object ? object[key] : () => {} }));
    }
    return elements.get(selector);
  };
  const connected = { connected: true, error: null, board_status: { led_available: true, led_on: false },
    hardware: { board_model: "Pico 2", mcu: "RP2350", capabilities: 2,
      ram_reserved_bytes: 121008, ram_total_bytes: 532480 },
    settings_supported: true, settings: { system_clock_mhz: 200, sdcard_profile: 0 },
    memory: { mode: "spi" }, msc: { enabled: false, transition_pending: false }, self_test: null };
  let statusFails = false;
  let release;
  let gate;
  const requestedUrls = [];
  const savedSettings = [];
  let saveFails = false;
  let readbackMismatch = false;
  let rebootFails = false;
  const saved = { document: globalThis.document, window: globalThis.window, fetch: globalThis.fetch };
  globalThis.document = {
    querySelector: element,
    createElement: () => ({ value: "", textContent: "", children: [],
      append(...children) { this.children.push(...children); } }),
  };
  globalThis.window = { setInterval: callback => { tick = callback; }, confirm: () => true };
  const saveAndConfirm = async () => {
    handlers.get("#settings-save:click")();
    await handlers.get("#confirm-configuration:click")();
  };
  globalThis.fetch = async (url, options = {}) => {
    requestedUrls.push(url);
    if (url === "/api/boards") {
      return { ok: true, json: async () => ({
        boards: [
          { id: "board-a", product_name: "PicoForge", serial_number: "board-A", device_path: "/hid/a" },
          { id: "board-b", product_name: "PicoForge HID", serial_number: null, device_path: "/hid/b" },
        ],
        default_board_id: "board-a",
      }) };
    }
    if (url.startsWith("/api/status?board=")) {
      if (statusFails) throw new Error("status down");
      return { ok: true, json: async () => structuredClone(connected) };
    }
    if (url.startsWith("/api/settings?board=")) {
      const settings = JSON.parse(options.body);
      savedSettings.push(settings);
      return { ok: !saveFails, json: async () => ({ error: "save rejected",
        settings: readbackMismatch ? { ...settings, sdcard_profile: 99 } : settings }) };
    }
    if (url.startsWith("/api/reboot?board=")) {
      assert.deepEqual(JSON.parse(options.body), { confirm: true });
      return { ok: !rebootFails, status: rebootFails ? 503 : 200 };
    }
    if (url.startsWith("/api/msc/force-return?board=")) {
      assert.deepEqual(JSON.parse(options.body), { confirm_data_loss: true });
      return { ok: true, json: async () => ({ msc: { enabled: false } }) };
    }
    await gate;
    return { ok: false, status: 503, json: async () => ({ error: "HID command delivery failed" }) };
  };
  try {
    await import("../../web/js/app.mjs");
    await new Promise(resolve => setImmediate(resolve));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(typeof handlers.get("#reboot-control:click"), "function");
    assert.equal(typeof tick, "function", "startup must register status polling");
    assert.equal(elements.get("#connection-label").textContent, "Connected");
    assert.equal(elements.get("#settings-note").textContent, "");
    assert.equal(elements.get("#settings-note").hidden, true);
    renderStatus({ ...connected, memory: { configured: true, sample_valid: false, initialized: false } });
    assert.equal(elements.get("#memory-state").textContent, "Busy");
    assert.equal(elements.get("#memory-note").textContent, "");
    assert.equal(elements.get("#memory-speed").textContent, "--");
    for (const speedMode of ["high-speed", 2]) {
      renderStatus({ ...connected, memory: { configured: true, sample_valid: true, initialized: true,
        present: true, bus_clock_hz: 50000000, speed_mode: speedMode } });
      assert.equal(elements.get("#memory-state").textContent, "Ready");
      assert.equal(elements.get("#memory-speed").textContent, "50 MHz (High-Speed)");
    }
    const validStorage = { ...connected, filesystem_format: "exfat", memory: {
      configured: true, sample_valid: true, initialized: true, present: true, mode: "sdio",
      bus_clock_hz: 50000000, speed_mode: "high-speed", capacity_bytes: 127865454592,
      manufacturer: { id: 3, product_name: "SB128", product_revision: "8.0", manufacture_year: 2020,
        manufacture_month: 1, serial_number: 4089330880 },
    } };
    const busyStorage = { ...connected, memory: { configured: true, sample_valid: false } };
    renderStatus(validStorage);
    const detailIds = ["#memory-mode", "#memory-speed", "#memory-size", "#memory-filesystem",
      "#memory-manufacturer", "#memory-product", "#memory-serial"];
    const details = detailIds.map(id => elements.get(id).textContent);
    for (let sample = 0; sample < 3; sample++) {
      renderStatus(busyStorage);
      assert.equal(elements.get("#memory-state").textContent, "Busy");
      assert.equal(elements.get("#memory-note").textContent, "");
      assert.deepEqual(detailIds.map(id => elements.get(id).textContent), details);
    }
    for (const invalidation of [
      { connected: false },
      { ...connected, storage_stale: true },
      { ...connected, memory: { configured: true, sample_valid: true, presence_known: true, present: false } },
      { ...connected, memory: { configured: true, sample_valid: true, present: true, initialized: false } },
      { ...connected, msc: { enabled: true } },
      { ...connected, msc: { transition_pending: true } },
    ]) {
      renderStatus(validStorage);
      renderStatus(invalidation);
      renderStatus(busyStorage);
      assert.equal(elements.get("#memory-speed").textContent, "--", "invalidated details must not return");
    }
    renderStatus(connected);
    for (const transport of ["sdio", "spi"]) {
      renderStatus({ ...connected, board_configuration: {
        storage_transport: transport, storage_gpios: [2, 3, 4, 5, 6, 7], card_detect_gpio: 8,
        monitor_uart_index: 0, monitor_uart_tx_gpio: 0, monitor_uart_rx_gpio: 255, reset_cause: 2,
      } });
      const rows = id => elements.get(id).children.map(row => row.children.map(cell => cell.textContent));
      assert.deepEqual(rows("#board-uart"), [["TX", "RX"], ["GP0", "Unassigned"]]);
      assert.deepEqual(rows("#board-storage-pins"), transport === "spi"
        ? [["CLK", "MOSI", "MISO", "CS", "Card detect"], ["GP2", "GP3", "GP4", "GP5", "GP8"]]
        : [["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3", "Card detect"],
          ["GP2", "GP3", "GP4", "GP5", "GP6", "GP7", "GP8"]]);
    }
    renderStatus(connected);
    assert.equal(elements.get("#board-model").textContent, "Pico 2 (RP2350)");
    assert.equal(elements.get("#ram-usage").textContent, "118.2 / 520 KiB (22.7%)");
    const html = readFileSync(new URL("../../web/index.html", import.meta.url), "utf8");
    const identity = html.split('aria-label="Hardware identity"')[1].split("</section>")[0];
    assert.deepEqual([...identity.matchAll(/<strong id="([^"]+)"/g)].map(match => match[1]),
      ["product-name", "board-model", "serial", "identity-clock", "ram-usage", "firmware-version"]);
    assert.doesNotMatch(html, /id="ram-reserved"|id="ram-total"/);
    assert.doesNotMatch(html, /id="mcu"|Microcontroller/);
    assert.match(html, />Save and Reboot<\/button>/);
    assert.match(html, />Reboot Board<\/button>/);
    assert.match(html, /id="settings-title">Board configuration<\/h2>/);
    assert.doesNotMatch(html, /BOOT CONFIGURATION/);
    assert.doesNotMatch(html, /class="eyebrow"/);
    assert.doesNotMatch(html, /metric-product/);
    const diagnostics = html.split('<dl class="runtime-grid runtime-summary">')[1].split("</dl>")[0];
    assert.deepEqual([...diagnostics.matchAll(/<dt>([^<]+)<\/dt>/g)].map(match => match[1]),
      ["Uptime", "Service load (Core 0 / Core 1)", "Board temperature", "CLI port"]);
    const pins = html.match(/<details class="pin-map-disclosure"([^>]*)>([\s\S]*?)<\/details>/);
    assert.ok(pins);
    assert.doesNotMatch(pins[1], /\bopen\b/);
    assert.match(pins[2], /<summary>Pin configuration<\/summary>/);
    assert.doesNotMatch(pins[2], /board-reset-cause/);
    assert.match(html, /class="settings-controls"[\s\S]*?id="board-reset-cause"[\s\S]*?id="settings-note"/);
    for (const id of ["board-uart", "board-storage-pins"]) {
      assert.ok(pins[2].includes(`id="${id}"`));
    }
    assert.equal(elements.get("#board-select").value, "board-a");
    assert.equal(elements.get("#board-select").children.find(option => option.value === "board-a").textContent,
      "PicoForge / board-A");
    assert.equal(elements.get("#board-select").children.find(option => option.value === "board-b").textContent,
      "PicoForge / HID /hid/b");
    assert.ok(requestedUrls.includes("/api/status?board=board-a"));
    elements.get("#settings-clock").value = "250";
    handlers.get("#settings-clock:change")();
    const beforeCancel = requestedUrls.length;
    await handlers.get("#settings-save:click")();
    assert.equal(elements.get("#configuration-dialog").open, true);
    assert.match(elements.get("#configuration-dialog-profile").textContent, /250 MHz.*Standard/);
    handlers.get("#cancel-configuration:click")();
    assert.equal(elements.get("#configuration-dialog").open, false);
    await handlers.get("#reboot-control:click")();
    assert.equal(elements.get("#reboot-dialog").open, true);
    assert.match(elements.get("#reboot-consequence").textContent, /Unsaved.*lost/);
    handlers.get("#cancel-reboot:click")();
    handlers.get("#reboot-control:click")();
    handlers.get("#reboot-dialog:keydown")({ key: "Escape", preventDefault() {} });
    assert.equal(elements.get("#reboot-dialog").open, false);
    await handlers.get("#confirm-reboot:click")();
    await handlers.get("#confirm-configuration:click")();
    assert.equal(requestedUrls.length, beforeCancel);
    await saveAndConfirm();
    assert.deepEqual(savedSettings[0], { system_clock_mhz: 250, sdcard_profile: 0 });
    assert.equal(requestedUrls.at(-1), "/api/settings?board=board-a");
    assert.equal(requestedUrls.filter(url => url.startsWith("/api/reboot")).length, 0);
    elements.get("#settings-storage").value = "1";
    handlers.get("#settings-storage:change")();
    await saveAndConfirm();
    assert.deepEqual(savedSettings[1], { system_clock_mhz: 250, sdcard_profile: 1 });
    elements.get("#settings-clock").value = "200";
    handlers.get("#settings-clock:change")();
    for (const failure of ["save", "readback"]) {
      saveFails = failure === "save";
      readbackMismatch = failure === "readback";
      await saveAndConfirm();
      assert.equal(requestedUrls.at(-1), "/api/settings?board=board-a", "failed save must not reboot");
      assert.match(elements.get("#settings-note").textContent, /Save failed/);
      assert.equal(elements.get("#settings-note").hidden, false);
    }
    saveFails = false;
    readbackMismatch = false;
    await saveAndConfirm();
    assert.match(elements.get("#settings-note").textContent, /Settings saved. Reboot requested/);
    rebootFails = true;
    await handlers.get("#reboot-control:click")();
    await handlers.get("#confirm-reboot:click")();
    assert.match(elements.get("#settings-note").textContent, /Reboot failed/);
    rebootFails = false;
    await handlers.get("#reboot-control:click")();
    await handlers.get("#confirm-reboot:click")();
    assert.equal(requestedUrls.at(-1), "/api/reboot?board=board-a");
    for (const name of ["#led-control:click", "#msc-control:click", "#self-test-start:click"]) {
      gate = new Promise(resolve => { release = resolve; });
      statusFails = false;
      tick();
      await new Promise(resolve => setImmediate(resolve));
      const pending = handlers.get(name)?.();
      statusFails = true;
      tick();
      await new Promise(resolve => setImmediate(resolve));
      release();
      await assert.doesNotReject(async () => pending, name);
    }
    assert.ok(requestedUrls.includes("/api/led?board=board-a"));
    statusFails = false;
    connected.msc = { enabled: true, force_return_supported: true, transition_pending: false };
    await tick();
    const beforeReturn = requestedUrls.length;
    await handlers.get("#msc-control:click")();
    assert.equal(elements.get("#return-dialog").open, true);
    assert.equal(requestedUrls.length, beforeReturn);
    handlers.get("#cancel-return:click")();
    await handlers.get("#confirm-return:click")();
    assert.equal(requestedUrls.length, beforeReturn);
    await handlers.get("#msc-control:click")();
    handlers.get("#return-dialog:keydown")({ key: "Escape", preventDefault() {} });
    assert.equal(elements.get("#return-dialog").open, false);
    assert.equal(requestedUrls.length, beforeReturn);
    await handlers.get("#msc-control:click")();
    await handlers.get("#confirm-return:click")();
    assert.equal(requestedUrls.at(-1), "/api/msc/force-return?board=board-a");
    assert.equal(elements.get("#return-dialog").open, false);
    elements.get("#board-select").value = "board-b";
    handlers.get("#board-select:change")();
    await new Promise(resolve => setImmediate(resolve));
    assert.ok(requestedUrls.includes("/api/status?board=board-b"));
  } finally {
    Object.assign(globalThis, saved);
  }
});
