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
    runtime: { led_available: true, led_on: false },
    hardware: { product_name: "PicoForge", board_model: "Pico 2 W", mcu: "RP2350",
      capabilities: 128 | 16 | 2, ram_reserved_bytes: 121008, ram_total_bytes: 532480 },
    settings_supported: true,
    board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0,
      storage_gpios: [2,3,4,5,6,7], monitor_uart_index: 255 },
    memory: { mode: "spi", initialized: true, filesystem_format: "exfat",
      msc: { enabled: false, transition_pending: false } },
    storage_stale: false,
    self_test: {sequence: 1, active: false, state: "idle", written_bytes: 0, target_bytes: 0,
      read_bytes: 0, write_elapsed_ms: 0, read_elapsed_ms: 0},
    wireless: { ssid: "", wifi_enabled: false, password_configured: false, ble_enabled: false,
      ble_key_configured: false, tcp_cli_enabled: true }
  };
  window.sharedBrowserState = {connected, unknown: false, scans: 0, leds: 0, tests: 0,
    ledUncertain: false, mismatch: false, cleanupFailure: false, cleanups: 0};
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
    if (url.pathname === "/api/status") return response(connected);
    if (url.pathname === "/api/led") {
      state.leds++;
      if (state.ledUncertain) throw new DOMException("lost reply", "TimeoutError");
      return response({});
    }
    if (url.pathname === "/api/reboot") return response({});
    if (url.pathname === "/api/self-test") {state.tests++; return response({}, 202);}
    if (url.pathname === "/api/settings") {
      const settings = JSON.parse(options.body);
      return response({settings: {...settings, sdcard_profile: state.mismatch ? 99 : settings.sdcard_profile}});
    }
    if (url.pathname === "/api/wireless" && options.method === "POST") {
      const request = JSON.parse(options.body);
      return response({
        settings: request.board, reboot_pending: true,
        wireless: {ssid: request.ssid, ssid_hex: request.ssid_hex ??
          [...new TextEncoder().encode(request.ssid)].map(byte => byte.toString(16).padStart(2, "0")).join(""),
          wifi_enabled: request.wifi_enabled, ble_enabled: request.ble_enabled,
          password_configured: true, ble_key_configured: Boolean(request.ble_key),
          tcp_cli_enabled: request.tcp_cli_enabled}
      });
    }
    if (url.pathname === "/api/wireless/test") return response({
      test: { remaining_seconds: 600, ipv4: "192.0.2.2" }
    });
    if (url.pathname === "/api/wireless/scan") return response({
      networks: [{ ssid: "browser-network", ssid_hex: "62726f777365722d6e6574776f726b", secured: true, rssi: -40 }]
    });
    if (url.pathname === "/api/wireless/cancel" && connected.wireless_cleanup_required) {
      state.cleanups++;
      if (state.cleanupFailure) return response({error: "Cleanup failed", uncertain: false}, 409);
      connected.wireless_cleanup_required = false;
      return response({cancelled: true});
    }
    if (url.pathname === "/api/wireless/cancel" && ++cancelAttempts === 1) {
      return response({ error: "temporary failure" }, 409);
    }
    if (url.pathname === "/api/wireless/cancel") return response({cancelled: true});
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
  radio.focus();
  radio.value = "configure";
  radio.dispatchEvent(new Event("input", { bubbles: true }));
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
  check(page.activeElement === radio, "focus was not restored after a successful test");

  radio.focus();
  radio.value = "configure";
  radio.dispatchEvent(new Event("input", { bubbles: true }));
  const cancel = new Event("cancel", { cancelable: true });
  dialog.dispatchEvent(cancel);
  await waitFor(() => page.querySelector("#wifi-status").textContent.includes("temporary failure"),
    "failed cancellation was not reported");
  check(cancel.defaultPrevented && dialog.open, "failed keyboard cancellation closed the dialog");
  check(page.querySelector("#wifi-cancel").textContent === "Retry clearing test", "retry is not offered");
  page.querySelector("#wifi-cancel").click();
  await waitFor(() => !dialog.open, "cancellation retry did not close the dialog");
  check(page.activeElement === radio, "focus was not restored after cancellation retry");
});

await run("replacement Control key cannot receive stale clipboard feedback", async () => {
  const radio = page.querySelector("#settings-ble");
  radio.focus();
  radio.value = "configure";
  radio.dispatchEvent(new Event("input", { bubbles: true }));
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

if (results) results.dataset.done = "true";
globalThis.sharedBrowserResults = hostedResults;
