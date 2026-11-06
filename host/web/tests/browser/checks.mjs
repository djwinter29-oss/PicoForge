// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

const results = document.querySelector("#results");
const dashboard = document.querySelector("#dashboard");
const hostedResults = [];
const report = (name, error) => {
  hostedResults.push({ name, error: error?.message ?? null });
  if (!results) return;
  const item = document.createElement("li");
  item.textContent = `${error ? "FAIL" : "PASS"} ${name}${error ? `: ${error.message}` : ""}`;
  item.dataset.result = error ? "fail" : "pass";
  results.append(item);
};
const check = (condition, message) => {
  if (!condition) throw new Error(message);
};
const waitFor = async (condition, message) => {
  const deadline = Date.now() + 3000;
  while (!condition() && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
  check(condition(), message);
};
const run = async (name, action) => {
  try {
    await action();
    report(name);
  } catch (error) {
    report(name, error);
  }
};

let page;
let dashboardWindow;
if (globalThis.sharedBrowserHosted) {
  page = document;
  dashboardWindow = window;
} else {
const source = await (await fetch("../../src/index.html")).text();
const mock = `<script>
  const response = (body, status = 200) => ({
    ok: status >= 200 && status < 300,
    status,
    json: async () => structuredClone(body)
  });
  let cancelAttempts = 0;
  const connected = {
    connected: true,
    command_outcome_uncertain: false, reboot_pending: false, telemetry_stale: false,
    command_resync_supported: true,
    runtime: { led_available: true, led_on: false },
    hardware: { product_name: "PicoForge", board_model: "Pico 2 W", mcu: "RP2350",
      capabilities: 128 | 16 | 2, ram_reserved_bytes: 121008, ram_total_bytes: 532480 },
    settings_supported: true,
    board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0,
      storage_gpios: [2,3,4,5,6,7], monitor_uart_index: 255 },
    memory: { mode: "spi", initialized: true, filesystem_format: "exfat",
      msc: { enabled: false, transition_pending: false } },
    storage_stale: false, self_test_stale: false,
    self_test: {sequence: 1, active: false, state: "idle", written_bytes: 0, target_bytes: 0,
      read_bytes: 0, write_elapsed_ms: 0, read_elapsed_ms: 0},
    wireless: { ssid: "", wifi_enabled: false, password_configured: false, ble_enabled: false,
      ble_key_configured: false, tcp_cli_enabled: true }
  };
  window.sharedBrowserState = {connected, unknown: false, scans: 0, leds: 0, tests: 0,
    savedWireless: structuredClone(connected.wireless), testId: null,
    ledUncertain: false, mismatch: false, cleanupFailure: false, cleanups: 0,
    scanInvalid: false, wirelessScans: 0, resyncs: 0, failedCommand: null, noResultCommand: null, mscRequests: 0,
    statusReads: 0, malformedStatus: null, rebootUncertain: false, wirelessRequests: [], settingsRequests: []};
  window.fetch = async (input, options = {}) => {
    const url = new URL(input, document.baseURI);
    const state = window.sharedBrowserState;
    if (url.pathname === "/api/boards") {
      state.scans++;
      return response({
        boards: [{ id: "browser-board", serial_number: "browser-board", device_path: "/hid/browser" }],
        default_board_id: "browser-board"
      });
    }
    if (url.pathname === "/api/status" && state.unknown)
      return response({error: "unknown board; rescan and select a listed board"}, 404);
    if (url.pathname === "/api/status") {
      state.statusReads++;
      return response(state.malformedStatus ?? connected);
    }
    if (url.pathname === "/api/control/resync") {
      state.resyncs++;
      if (state.failedCommand !== null) {
        const opcode = state.failedCommand;
        state.failedCommand = null;
        connected.command_outcome_uncertain = false;
        return response({uncertain: false, control_result: {opcode, status: "failed", error: "storage_busy"}});
      }
      return response({uncertain: false});
    }
    if (url.pathname === "/api/msc" || url.pathname === "/api/self-test"
      && (state.failedCommand === 4 || state.noResultCommand === 4)) {
      if (url.pathname === "/api/msc") state.mscRequests++;
      else state.tests++;
      if (state.noResultCommand === null) connected.command_outcome_uncertain = true;
      throw new TypeError("lost command response");
    }
    if (url.pathname === "/api/led") {
      state.leds++;
      if (state.ledUncertain) throw new DOMException("lost reply", "TimeoutError");
      return response({});
    }
    if (url.pathname === "/api/reboot") {
      if (state.rebootUncertain) throw new TypeError("lost reboot reply");
      return response({});
    }
    if (url.pathname === "/api/self-test") {state.tests++; return response({}, 202);}
    if (url.pathname === "/api/settings") {
      const settings = JSON.parse(options.body);
      state.settingsRequests.push(settings);
      return response({settings: {...settings, sdcard_profile: state.mismatch ? 99 : settings.sdcard_profile}});
    }
    if (url.pathname === "/api/wireless" && options.method === "POST") {
      const request = JSON.parse(options.body);
      state.wirelessRequests.push(request);
      if (request.use_tested_wifi && (request.test_id !== state.testId
        || request.test_generation !== connected.wireless.test_generation))
        return response({error: "The acknowledged Wi-Fi test was replaced; test again", uncertain: false}, 409);
      const ssid = request.ssid ?? state.savedWireless.ssid;
      state.savedWireless = {ssid, ssid_hex: request.ssid_hex ?? (request.ssid === undefined
        ? state.savedWireless.ssid_hex
        : [...new TextEncoder().encode(ssid)].map(byte => byte.toString(16).padStart(2, "0")).join("")),
        wifi_enabled: request.wifi_enabled, ble_enabled: request.ble_enabled,
        ble_key_configured: request.ble_key === undefined
          ? state.savedWireless.ble_key_configured : Boolean(request.ble_key),
        tcp_cli_enabled: request.tcp_cli_enabled, pair_tested: false,
        ssid_staged: false, password_staged: false, key_staged: false};
      connected.wireless = structuredClone(state.savedWireless);
      return response({
        settings: request.board, reboot_pending: true,
        wireless: connected.wireless
      });
    }
    if (url.pathname === "/api/wireless/test") {
      const request = JSON.parse(options.body);
      const wireless = connected.wireless;
      if (!wireless.ssid_staged && !wireless.password_staged)
        state.savedWireless = structuredClone(wireless);
      const test_generation = (wireless.test_generation ?? 0) % 255 + 1;
      state.testId = test_generation.toString(16).padStart(32, "0");
      Object.assign(wireless, {ssid: request.ssid, ssid_hex: request.ssid_hex ??
        [...new TextEncoder().encode(request.ssid)].map(byte => byte.toString(16).padStart(2, "0")).join(""),
        test_generation, wifi_state: "test_joined", pair_tested: true, ssid_staged: true, password_staged: true});
      return response({test: { remaining_seconds: 600, ipv4: "192.0.2.2", test_generation, test_id: state.testId }});
    }
    if (url.pathname === "/api/wireless/scan") {
      state.wirelessScans++;
      return response({networks: state.scanInvalid ? [null]
        : [{ ssid: "browser-network", ssid_hex: "62726f777365722d6e6574776f726b", secured: true, rssi: -40 }]});
    }
    if (url.pathname === "/api/wireless/cancel" && connected.wireless_cleanup_required) {
      state.cleanups++;
      if (state.cleanupFailure) return response({error: "Cleanup failed", uncertain: false}, 409);
      connected.wireless_cleanup_required = false;
      connected.wireless = structuredClone(state.savedWireless);
      state.testId = null;
      return response({cancelled: true});
    }
    if (url.pathname === "/api/wireless/cancel" && ++cancelAttempts === 1) {
      return response({ error: "temporary failure" }, 409);
    }
    if (url.pathname === "/api/wireless/cancel") {
      connected.wireless = structuredClone(state.savedWireless);
      state.testId = null;
      return response({cancelled: true});
    }
    return response({ error: "Unexpected browser-check request" }, 404);
  };
</script>
<script type="module" src="js/app.mjs"></script>`;
const html = source
  .replace("<head>", `<head><base href="${new URL("../../src/", import.meta.url)}">`)
  .replace('<script type="module" src="/js/app.mjs"></script>', mock)
  .replaceAll('href="/', `href="${new URL("../../src/", import.meta.url)}`)
  .replace('src="js/app.mjs"', `src="${new URL("../../src/js/app.mjs", import.meta.url)}"`);
dashboard.style.width = "1000px";
dashboard.style.height = "800px";
dashboard.srcdoc = html;
await new Promise((resolve, reject) => {
  dashboard.addEventListener("load", resolve, { once: true });
  dashboard.addEventListener("error", reject, { once: true });
});
page = dashboard.contentDocument;
dashboardWindow = dashboard.contentWindow;
}
await waitFor(() => page.querySelector("#connection-label").textContent === "Connected",
  "mock board did not connect");

const configureRadio = radio => {
  const button = page.querySelector(`#${radio.id === "settings-wifi" ? "wifi" : "ble"}-configure`);
  button.focus();
  button.click();
};

await run("command lifecycle gates, restart recovery, and confirmation focus", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  const reconnect = async () => {
    state.connected.connected = false;
    refresh();
    await waitFor(() => el("connection-label").textContent === "Disconnected", "disconnect was not rendered");
    state.connected.connected = true;
    refresh();
    await waitFor(() => !el("led-control").disabled, "reconnection did not release command gates");
  };
  await waitFor(() => !el("led-control").disabled, "initial commands did not become ready");
  const scans = state.scans;
  state.unknown = true;
  refresh();
  await waitFor(() => state.scans > scans, "production 404 did not trigger a rescan");
  state.unknown = false;
  refresh();
  await waitFor(() => !el("led-control").disabled, "404 recovery did not restore commands");

  el("reboot-control").click();
  el("confirm-reboot").click();
  await waitFor(() => el("settings-note").textContent.includes("Reboot requested"), "reboot was not acknowledged");
  check(el("led-control").disabled, "acknowledged reboot left LED enabled");
  refresh();
  await new Promise(resolve => setTimeout(resolve, 30));
  check(el("led-control").disabled, "old status cleared the local reboot gate");
  await reconnect();

  state.ledUncertain = true;
  el("led-control").click();
  await waitFor(() => el("status-message").textContent.includes("uncertain"), "lost LED reply was not retained");
  el("self-test-size").value = "64";
  el("self-test-size").dispatchEvent(new Event("input", {bubbles: true}));
  check(el("self-test-start").disabled, "size edit bypassed the command gate");
  const tests = state.tests;
  el("self-test-start").click();
  check(state.tests === tests, "benchmark request escaped the gate");
  state.ledUncertain = false;
  state.connected.runtime.led_on = true;
  await reconnect();

  state.mismatch = true;
  el("settings-clock").value = "250";
  el("settings-clock").dispatchEvent(new Event("change", {bubbles: true}));
  el("settings-save").click();
  el("confirm-configuration").click();
  await waitFor(() => el("settings-note").textContent.includes("uncertain"), "mismatched save was not unresolved");
  check(el("settings-save").disabled && el("led-control").disabled, "mismatched save permits another write");
  state.mismatch = false;
  state.connected.board_configuration.system_clock_mhz = 250;
  await reconnect();

  state.connected.wireless_cleanup_required = true;
  state.cleanupFailure = true;
  refresh();
  await waitFor(() => !el("command-resync").hidden, "cleanup recovery was not offered");
  check(el("command-resync").textContent === "Clear Wi-Fi test", "known cleanup failure incorrectly asks for Re-sync");
  check(el("led-control").disabled, "cleanup requirement permits unrelated writes");
  el("command-resync").click();
  await waitFor(() => el("wireless-summary").textContent.includes("Cleanup failed"), "cleanup failure was hidden");
  check(!el("command-resync").disabled, "explicit cleanup retry is unavailable");
  state.cleanupFailure = false;
  el("command-resync").click();
  await waitFor(() => !el("led-control").disabled, "confirmed cleanup did not restore commands");
  check(state.cleanups === 2, "cleanup was automatically replayed");

  const opener = page.querySelector("#msc-control");
  const dialog = page.querySelector("#share-dialog");
  opener.focus();
  opener.click();
  check(dialog.open, "confirmation dialog did not open");
  check(dialog.contains(page.activeElement), "focus did not move into the dialog");
  const escape = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
  page.activeElement.dispatchEvent(escape);
  check(escape.defaultPrevented && !dialog.open, "Escape did not close the dialog");
  check(page.activeElement === opener, "focus was not restored to the opener");
});

await run("narrow viewport does not overflow horizontally", async () => {
  if (dashboard) dashboard.style.width = "360px";
  await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  check(page.documentElement.scrollWidth <= page.documentElement.clientWidth, "dashboard overflows horizontally");
});

await run("Wi-Fi cancellation failure remains open and retry succeeds", async () => {
  const radio = page.querySelector("#settings-wifi");
  const dialog = page.querySelector("#wifi-dialog");
  configureRadio(radio);
  check(dialog.open, "Wi-Fi dialog did not open");
  check(dialog.contains(page.activeElement), "focus did not move into the Wi-Fi dialog");
  page.querySelector("#wifi-scan").click();
  await waitFor(() => page.querySelector(".wifi-network"), "scan did not populate the network list");
  const network = page.querySelector(".wifi-network");
  network.click();
  check(network.getAttribute("aria-pressed") === "true", "selected network is not announced as pressed");
  const bounds = dialog.getBoundingClientRect();
  check(bounds.left >= 0 && bounds.right <= page.documentElement.clientWidth,
    "Wi-Fi dialog overflows the narrow viewport");
  check(dialog.scrollWidth <= dialog.clientWidth, "Wi-Fi dialog content overflows horizontally");
  page.querySelector("#wifi-other").click();
  check(network.getAttribute("aria-pressed") === "false", "manual entry did not clear network selection");
  const ssid = page.querySelector("#wifi-ssid");
  ssid.value = "browser-check";
  ssid.dispatchEvent(new Event("input", { bubbles: true }));
  page.querySelector("#wifi-connect").click();
  await waitFor(() => !dialog.open, "Wi-Fi test did not finish and close the dialog");
  check(page.activeElement === page.querySelector("#wifi-configure"), "focus was not restored after a successful test");

  configureRadio(radio);
  const cancel = new Event("cancel", { cancelable: true });
  dialog.dispatchEvent(cancel);
  await waitFor(() => page.querySelector("#wifi-status").textContent.includes("temporary failure"),
    "failed cancellation was not reported");
  check(cancel.defaultPrevented && dialog.open, "failed keyboard cancellation closed the dialog");
  check(page.querySelector("#wifi-cancel").textContent === "Retry clearing test", "retry is not offered");
  page.querySelector("#wifi-cancel").click();
  await waitFor(() => !dialog.open, "cancellation retry did not close the dialog");
  check(page.activeElement === page.querySelector("#wifi-configure"), "focus was not restored after cancellation retry");
});

await run("browser-only uncertainty is recoverable inside the Wi-Fi dialog without replay", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const radio = el("settings-wifi");
  configureRadio(radio);
  state.scanInvalid = true;
  const scans = state.wirelessScans;
  el("wifi-scan").click();
  await waitFor(() => !el("wifi-resync").hidden && !el("wifi-resync").disabled, "modal Re-sync was not offered");
  check(el("wifi-dialog").open, "uncertain scan unexpectedly closed the dialog");
  check(el("led-control").disabled, "local uncertainty permits unrelated writes");
  check(!state.connected.command_outcome_uncertain, "scenario must have browser-only uncertainty");
  page.dispatchEvent(new Event("visibilitychange"));
  await new Promise(resolve => setTimeout(resolve, 30));
  check(el("status-message").textContent.includes("Re-sync"), "polling erased the recovery explanation");
  el("wifi-resync").click();
  await waitFor(() => !el("led-control").disabled, "explicit host confirmation did not clear local uncertainty");
  check(state.resyncs === 1 && state.wirelessScans === scans + 1, "recovery replayed the scan");
  check(el("wifi-dialog").open && el("wifi-resync").hidden, "recovery lost modal ownership");
  state.scanInvalid = false;
  el("wifi-cancel").click();
  await waitFor(() => !el("wifi-dialog").open, "recovered dialog could not close");
});

await run("failed terminal results release benchmark and storage gates without automatic retry", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  for (const opcode of [4, 3]) {
    state.failedCommand = opcode;
    const requests = opcode === 4 ? state.tests : state.mscRequests;
    if (opcode === 4) el("self-test-start").click();
    else {
      el("msc-control").click();
      el("confirm-share").click();
    }
    await waitFor(() => !el("command-resync").hidden && !el("command-resync").disabled,
      "lost command did not offer Re-sync");
    el("command-resync").click();
    await waitFor(() => el("command-resync").hidden && !el("msc-control").disabled && !el("self-test-start").disabled,
      "confirmed failure did not release its pending marker");
    page.dispatchEvent(new Event("visibilitychange"));
    await new Promise(resolve => setTimeout(resolve, 30));
    check(el("status-message").textContent.includes("storage_busy"), "polling erased the final failure reason");
    check((opcode === 4 ? state.tests : state.mscRequests) === requests + 1, "failed command was replayed");
  }
});

await run("replacement Control key cannot receive stale clipboard feedback", async () => {
  const radio = page.querySelector("#settings-ble");
  configureRadio(radio);
  let complete;
  const clipboard = { writeText: () => new Promise(resolve => { complete = resolve; }) };
  Object.defineProperty(dashboardWindow.navigator, "clipboard", { configurable: true, value: clipboard });
  page.querySelector("#ble-generate").click();
  page.querySelector("#ble-copy").click();
  check(typeof complete === "function", "clipboard operation did not start");
  page.querySelector("#ble-generate").click();
  const message = page.querySelector("#ble-status").textContent;
  complete();
  await new Promise(resolve => setTimeout(resolve, 0));
  check(page.querySelector("#ble-status").textContent === message, "stale clipboard success replaced current feedback");
  page.querySelector("#ble-cancel").click();
});

await run("clean-host no-result recovery waits for reconnect and fresh status without replay", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  for (const opcode of [4, 3]) {
    state.noResultCommand = opcode;
    const requests = opcode === 4 ? state.tests : state.mscRequests;
    if (opcode === 4) el("self-test-start").click();
    else {
      el("msc-control").click();
      el("confirm-share").click();
    }
    await waitFor(() => !el("command-resync").hidden && !el("command-resync").disabled,
      "browser-only failure did not offer Re-sync");
    if (opcode === 4) {
      for (const sequence of [255, 0, 1]) {
        state.connected.self_test.sequence = sequence;
        refresh();
        await new Promise(resolve => setTimeout(resolve, 30));
        check(!el("command-resync").hidden && el("led-control").disabled && el("self-test-start").disabled,
          "routine report sequence advancement cleared unknown benchmark delivery");
      }
    }
    check(!state.connected.command_outcome_uncertain, "fixture should not invent a host-side command result");
    el("command-resync").click();
    await waitFor(() => el("status-message").textContent.includes("Disconnect and reconnect this board"),
      "no-result recovery did not explain its reconnect requirement");
    check(el("self-test-start").disabled && el("msc-control").disabled && el("led-control").disabled,
      "clean host incorrectly released unresolved command gates");
    state.connected.connected = false;
    refresh();
    await waitFor(() => el("settings-note").textContent === "Waiting for a PicoForge connection.",
      "disconnect not observed");
    state.connected.connected = true;
    state.connected.storage_stale = true;
    refresh();
    await waitFor(() => el("status-message").textContent.includes("Disconnect and reconnect this board"),
      "reconnecting status did not retain the obligation");
    check(el("led-control").disabled, "stale reconnect released the gate");
    state.connected.storage_stale = false;
    refresh();
    await waitFor(() => !el("led-control").disabled && !el("msc-control").disabled
      && !el("self-test-start").disabled, "fresh status after reconnect did not release local pending state");
    check((opcode === 4 ? state.tests : state.mscRequests) === requests + 1, "recovery replayed a command");
    state.noResultCommand = null;
  }
});

await run("stale benchmark status blocks storage writes without blocking LED or read-only recovery", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const saved = state.connected.self_test;
  state.connected.hardware.capabilities |= 32;
  state.connected.self_test_stale = true;
  state.connected.self_test = null;
  page.dispatchEvent(new Event("visibilitychange"));
  await waitFor(() => el("msc-control").disabled && el("reboot-control").disabled,
    "stale benchmark section left storage-dependent commands available");
  check(el("settings-note").textContent.includes("Waiting for fresh benchmark status")
    && el("msc-note").textContent.includes("Waiting for fresh benchmark status"),
    "disabled commands do not explain the missing benchmark status");
  check(!el("led-control").disabled, "unrelated LED control was unnecessarily blocked");
  state.connected.self_test = saved;
  state.connected.self_test_stale = false;
  page.dispatchEvent(new Event("visibilitychange"));
  await waitFor(() => !el("msc-control").disabled && !el("reboot-control").disabled,
    "fresh benchmark status did not restore command admission");
  state.connected.hardware.capabilities &= ~32;
});

await run("delayed pre-command status cannot release a confirmed benchmark observation fence", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  const original = dashboardWindow.fetch;
  let release, reads = 0;
  dashboardWindow.fetch = async (input, options) => {
    if (new URL(input, page.baseURI).pathname === "/api/status") {
      reads++;
      if (reads === 1) {
        const stale = structuredClone(state.connected);
        stale.self_test.sequence = (stale.self_test.sequence + 1) & 255;
        return new Promise(resolve => {
          release = () => resolve({ ok: true, json: async () => stale });
        });
      }
    }
    return original(input, options);
  };
  try {
    refresh();
    await waitFor(() => release, "status request was not held before the command");
    const requests = state.tests;
    el("self-test-start").click();
    await waitFor(() => state.tests === requests + 1, "benchmark request was not sent");
    await new Promise(resolve => setTimeout(resolve, 30));
    release();
    await waitFor(() => reads >= 2, "obsolete response did not trigger a replacement status request");
    await new Promise(resolve => setTimeout(resolve, 30));
    check(el("self-test-start").disabled && el("msc-control").disabled, "old idle status released command gates");
    el("self-test-start").click();
    check(state.tests === requests + 1, "the fenced command was sent again");
    state.connected.self_test.sequence = (state.connected.self_test.sequence + 1) & 255;
    refresh();
    await waitFor(() => !el("self-test-start").disabled, "a newer post-command report did not release the fence");
  } finally {
    release?.();
    dashboardWindow.fetch = original;
  }
});

await run("testing another Wi-Fi network survives candidate polls and saves once before the reboot gate", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "disconnect was not observed");
  state.connected.board_configuration.radios = 1;
  Object.assign(state.connected.wireless, { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true,
    ble_enabled: false, ble_key_configured: false, tcp_cli_enabled: true, pair_tested: false });
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("settings-tcp-cli").disabled, "wireless baseline did not initialize");
  let previousCandidate;
  for (const ssid of ["different-browser-network", "replacement-browser-network"]) {
    const radio = el("settings-wifi");
    configureRadio(radio);
    el("wifi-ssid").value = ssid;
    el("wifi-ssid").dispatchEvent(new Event("input", { bubbles: true }));
    el("wifi-connect").click();
    await waitFor(() => !el("wifi-dialog").open && el("wireless-summary").textContent.includes(ssid),
      "network testing did not complete");
    const latestCandidate = structuredClone(state.connected.wireless);
    if (previousCandidate) {
      state.connected.wireless = previousCandidate;
      refresh();
      await waitFor(() => el("wireless-summary").textContent.includes("Waiting for status matching"),
        "cached previous candidate was not held behind a recoverable gate");
      check(el("settings-save").disabled && !el("wireless-summary").textContent.includes("Reload"),
        "old candidate caused a permanent conflict or enabled Save");
      state.connected.wireless = latestCandidate;
    }
    const reads = state.statusReads;
    refresh();
    await waitFor(() => state.statusReads > reads, "candidate SSID was not polled");
    await new Promise(resolve => setTimeout(resolve, 30));
    check(!el("settings-save").disabled && !el("wireless-summary").textContent.includes("configuration changed"),
      "own candidate was mistaken for a changed saved network");
    previousCandidate = latestCandidate;
  }
  const saves = state.wirelessRequests.length;
  el("settings-save").click();
  el("confirm-configuration").click();
  await waitFor(() => el("settings-note").textContent.includes("Settings saved"), "tested pair was not saved");
  check(state.wirelessRequests.length === saves + 1, "Save did not send exactly one request");
  const request = state.wirelessRequests.at(-1);
  check(request.use_tested_wifi && request.ssid === "replacement-browser-network",
    "Save used the saved network rather than the tested pair");
  check(request.test_id === state.testId && request.test_generation === previousCandidate.test_generation,
    "Save did not carry the acknowledged test identity");
  check(el("led-control").disabled && el("settings-save").disabled, "successful Save lost its reboot obligation");
});

await run("malformed status preserves a key draft and cannot manufacture unknown-reboot recovery", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "disconnect was not observed");
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("led-control").disabled, "connected status did not restore commands");
  const radio = el("settings-ble");
  configureRadio(radio);
  el("ble-generate").click();
  const key = el("ble-secret").value;
  el("ble-cancel").click();
  state.rebootUncertain = true;
  el("reboot-control").click();
  el("confirm-reboot").click();
  await waitFor(() => !el("command-resync").hidden, "lost reboot delivery did not require recovery");
  state.malformedStatus = {};
  refresh();
  await waitFor(() => el("status-message").textContent.includes("Status unavailable: Invalid status"),
    "malformed status was silently rendered as disconnected");
  check(el("ble-secret").value === key, "invalid status erased the key draft");
  state.malformedStatus = null;
  refresh();
  await waitFor(() => !el("status-message").textContent.includes("Invalid status"), "valid status did not recover");
  check(el("led-control").disabled && !el("command-resync").hidden && el("ble-secret").value === key,
    "invalid status manufactured a disconnect or discarded uncertainty");
  state.rebootUncertain = false;
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "real disconnect was not observed");
  check(el("ble-secret").value === "", "real disconnect did not clear the key");
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("led-control").disabled, "real reconnect did not release reboot recovery");
});

await run("wireless polls refresh clean settings and preserve conflicting drafts with an explicit Save gate", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "draft reset did not observe disconnect");
  state.connected.board_configuration.radios = 1;
  Object.assign(state.connected.wireless, { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true,
    ble_enabled: false, ble_key_configured: false, tcp_cli_enabled: true });
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("settings-tcp-cli").disabled, "wireless baseline did not initialize");
  state.connected.wireless.tcp_cli_enabled = false;
  refresh();
  await waitFor(() => el("settings-tcp-cli").value === "0", "clean form kept an obsolete TCP CLI setting");
  check(el("settings-save").disabled, "baseline synchronization created a spurious draft");
  const radio = el("settings-ble");
  configureRadio(radio);
  el("ble-generate").click();
  const key = el("ble-secret").value;
  el("ble-cancel").click();
  state.connected.wireless.ssid = "changed";
  state.connected.wireless.ssid_hex = "6368616e676564";
  refresh();
  await waitFor(() => el("wireless-summary").textContent.includes("configuration changed"),
    "changed authoritative settings did not expose a draft conflict");
  check(el("ble-secret").value === key && radio.value === "1", "polling overwrote the local key draft");
  check(el("settings-save").disabled && el("wireless-summary").textContent.includes("Discard wireless edits"),
    "conflicting draft can be saved without explicit recovery");
});

await run("another client's same-SSID test cannot replace the pair acknowledged by Save", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "draft reset missed disconnect");
  state.connected.board_configuration.radios = 1;
  state.connected.wireless = { ssid: "shared", ssid_hex: "736861726564", wifi_enabled: true,
    ble_enabled: false, ble_key_configured: false, tcp_cli_enabled: true,
    ssid_staged: false, password_staged: false, key_staged: false, pair_tested: false };
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("settings-tcp-cli").disabled, "wireless baseline did not initialize");
  const radio = el("settings-wifi");
  configureRadio(radio);
  el("wifi-ssid").value = "shared";
  el("wifi-ssid").dispatchEvent(new Event("input", { bubbles: true }));
  el("wifi-connect").click();
  await waitFor(() => !el("wifi-dialog").open && !el("settings-save").disabled, "test did not finish");
  const original = dashboardWindow.fetch;
  const cached = structuredClone(state.connected);
  const acknowledgedId = state.testId;
  dashboardWindow.fetch = async (input, options) => new URL(input, page.baseURI).pathname === "/api/status"
    ? { ok: true, status: 200, json: async () => structuredClone(cached) } : original(input, options);
  try {
    state.connected.wireless.test_generation++;
    state.testId = "f".repeat(32);
    el("settings-save").click();
    el("confirm-configuration").click();
    await waitFor(() => el("wifi-status").textContent.includes("test was replaced"),
      "Save did not reject another client's same-SSID test");
    check(state.wirelessRequests.at(-1).test_id === acknowledgedId,
      "Save did not send the browser's original test identity");
  } finally {
    dashboardWindow.fetch = original;
  }
  refresh();
  await waitFor(() => el("wireless-summary").textContent.includes("Waiting for status matching"),
    "replacement observation did not produce recoverable feedback");
  check(el("settings-save").disabled && !el("wireless-summary").textContent.includes("Reload"),
    "replacement caused a permanent conflict or allowed Save");
  configureRadio(radio);
  el("wifi-cancel").click();
  await waitFor(() => !el("wifi-dialog").open, "replacement cleanup did not complete");
});

await run("foreign staged credentials require cleanup and never join an unrelated Bluetooth save", async () => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "draft reset missed disconnect");
  state.connected.connected = true;
  state.connected.board_configuration.radios = 1;
  state.connected.wireless = { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true,
    ble_enabled: false, ble_key_configured: false, tcp_cli_enabled: true,
    ssid_staged: false, password_staged: false, key_staged: false, pair_tested: false };
  state.savedWireless = structuredClone(state.connected.wireless);
  refresh();
  await waitFor(() => !el("settings-tcp-cli").disabled, "wireless baseline did not initialize");
  const ble = el("settings-ble");
  configureRadio(ble);
  el("ble-generate").click();
  const key = el("ble-secret").value;
  el("ble-cancel").click();
  Object.assign(state.connected.wireless, { ssid: "foreign", ssid_hex: "666f726569676e",
    ssid_staged: true, password_staged: true, pair_tested: true, test_generation: 9 });
  refresh();
  await waitFor(() => el("wireless-summary").textContent.includes("another client staged"),
    "foreign staging was not distinguished from saved configuration");
  check(el("settings-save").disabled && el("ble-secret").value === key,
    "foreign staging enabled Save or discarded the local key");
  const wifi = el("settings-wifi");
  configureRadio(wifi);
  check(el("wifi-ssid").value !== "foreign", "foreign SSID became the saved baseline");
  el("wifi-cancel").click();
  await waitFor(() => !el("wifi-dialog").open, "foreign staging cleanup did not complete");
  refresh();
  await waitFor(() => !el("settings-save").disabled, "cleanup did not release the unrelated key save");
  el("settings-save").click();
  el("confirm-configuration").click();
  await waitFor(() => el("settings-note").textContent.includes("Settings saved"), "key-only save failed");
  const request = state.wirelessRequests.at(-1);
  check(request.ble_key === key, "cleanup discarded the locally generated key");
  check(["ssid", "ssid_hex", "password", "test_id", "test_generation"].every(field => !(field in request)),
    "unrelated key Save included temporary Wi-Fi credentials");
});

const radioBaseline = async (missing = false) => {
  const state = dashboardWindow.sharedBrowserState;
  const el = id => page.querySelector(`#${id}`);
  const refresh = () => page.dispatchEvent(new Event("visibilitychange"));
  state.connected.connected = false;
  refresh();
  await waitFor(() => el("connection-label").textContent === "Disconnected", "draft reset missed disconnect");
  state.connected.board_configuration = { ...state.connected.board_configuration,
    system_clock_mhz: 200, sdcard_profile: 0, radios: 1 };
  state.savedWireless = { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true,
    ble_enabled: false, ble_key_configured: false, tcp_cli_enabled: true,
    ssid_staged: false, password_staged: false, key_staged: false, pair_tested: false };
  state.connected.wireless = missing ? null : structuredClone(state.savedWireless);
  state.connected.wireless_error = missing ? "Feature 12 unavailable" : null;
  state.connected.connected = true;
  refresh();
  await waitFor(() => !el("led-control").disabled, "fresh connection did not restore commands");
  await waitFor(() => el("settings-wifi").disabled === missing, "wireless readiness did not reach the radio control");
  return { state, el, refresh };
};

await run("clock and storage drafts preserve remotely updated radios and credentials", async () => {
  for (const field of ["clock", "storage"]) {
    const { state, el, refresh } = await radioBaseline();
    const saves = state.settingsRequests.length;
    const credentials = state.wirelessRequests.length;
    el(`settings-${field}`).value = field === "clock" ? "250" : "1";
    el(`settings-${field}`).dispatchEvent(new Event("change", { bubbles: true }));
    state.connected.board_configuration.radios = 3;
    if (field === "clock") state.connected.board_configuration.sdcard_profile = 1;
    else state.connected.board_configuration.system_clock_mhz = 250;
    Object.assign(state.connected.wireless, { ble_enabled: true, ble_key_configured: true });
    refresh();
    await waitFor(() => el("settings-ble").value === "1", "untouched Bluetooth control stayed frozen");
    const reads = state.statusReads;
    refresh();
    await waitFor(() => state.statusReads > reads, "second baseline observation was not accepted");
    await new Promise(resolve => setTimeout(resolve, 30));
    check(el(`settings-${field}`).value === (field === "clock" ? "250" : "1"), "local edit was overwritten");
    check(!el("settings-save").disabled, "remote update manufactured a conflicting credential draft");
    el("settings-save").click();
    check(el("configuration-dialog-profile").textContent.includes("Bluetooth key: preserve"),
      "confirmation did not explain credential preservation");
    el("confirm-configuration").click();
    await waitFor(() => el("settings-note").textContent.includes("Settings saved"), "unrelated settings save failed");
    check(state.settingsRequests.length === saves + 1 && state.settingsRequests.at(-1).radios === 3,
      "unrelated Save reverted remote radio bits");
    check(state.settingsRequests.at(-1).system_clock_mhz === 250
      && state.settingsRequests.at(-1).sdcard_profile === 1, "an untouched setting did not follow accepted state");
    check(state.wirelessRequests.length === credentials, "unrelated Save staged or erased credentials");
  }
});

await run("missing first wireless status disables radios but allows a clock-only save preserving them", async () => {
  const { state, el } = await radioBaseline(true);
  const saves = state.settingsRequests.length;
  const credentials = state.wirelessRequests.length;
  check(el("settings-wifi").disabled && el("settings-ble").disabled, "radio editing bypassed missing status");
  check(el("settings-note").textContent.includes("Waiting for fresh wireless configuration"),
    "disabled radios did not explain the missing facts");
  el("settings-clock").value = "250";
  el("settings-clock").dispatchEvent(new Event("change", { bubbles: true }));
  el("settings-save").click();
  el("confirm-configuration").click();
  await waitFor(() => el("settings-note").textContent.includes("Settings saved"), "clock-only save was blocked");
  check(state.settingsRequests.length === saves + 1 && state.settingsRequests.at(-1).radios === 1,
    "clock-only save changed radio bits");
  check(state.wirelessRequests.length === credentials, "clock-only save touched credentials");
});

await run("forced radio-disable events cannot bypass readiness; recovery explicitly erases the credential pair", async () => {
  const { state, el, refresh } = await radioBaseline(true);
  const saves = state.settingsRequests.length;
  const credentials = state.wirelessRequests.length;
  el("settings-wifi").value = "0";
  for (const type of ["input", "change"])
    el("settings-wifi").dispatchEvent(new Event(type, { bubbles: true }));
  el("configuration-dialog").showModal();
  el("confirm-configuration").click();
  await new Promise(resolve => setTimeout(resolve, 30));
  check(state.settingsRequests.length === saves && state.wirelessRequests.length === credentials,
    "handler bypass saved a radio disable without credential facts");
  state.connected.wireless = structuredClone(state.savedWireless);
  state.connected.wireless_error = null;
  refresh();
  await waitFor(() => !el("settings-save").disabled, "fresh wireless status did not release explicit radio intent");
  check(el("settings-wifi").value === "0", "baseline recovery overwrote the explicit disable");
  el("settings-save").click();
  check(el("configuration-dialog-profile").textContent.includes("Wi-Fi credentials: erase"),
    "confirmation did not explain erasure");
  el("confirm-configuration").click();
  await waitFor(() => el("settings-note").textContent.includes("Settings saved"), "explicit disable did not save");
  const request = state.wirelessRequests.at(-1);
  check(state.wirelessRequests.length === credentials + 1 && request.ssid === "" && request.password === ""
    && request.wifi_enabled === false && !("ble_key" in request), "disable did not erase exactly its credential pair");
});

if (results) results.dataset.done = "true";
globalThis.sharedBrowserResults = hostedResults;
