const results = document.querySelector("#results");
const dashboard = document.querySelector("#dashboard");
const report = (name, error) => {
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

const response = (body, status = 200) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => structuredClone(body),
});
const source = await (await fetch("../index.html")).text();
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
    board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0 },
    memory: { mode: "spi", msc: { enabled: false, transition_pending: false } },
    storage_stale: false,
    self_test: null,
    wireless: { ssid: "", wifi_enabled: false, password_configured: false, ble_enabled: false,
      ble_key_configured: false, tcp_cli_enabled: true }
  };
  window.fetch = async (input, options = {}) => {
    const url = new URL(input, document.baseURI);
    if (url.pathname === "/api/boards") return response({
      boards: [{ id: "browser-board", serial_number: "browser-board", device_path: "/hid/browser" }],
      default_board_id: "browser-board"
    });
    if (url.pathname === "/api/status") return response(connected);
    if (url.pathname === "/api/wireless" && options.method === "POST") return response({
      wireless: { ...JSON.parse(options.body), password_configured: true }
    });
    if (url.pathname === "/api/wireless/test") return response({
      test: { remaining_seconds: 600, ipv4: "192.0.2.2" }
    });
    if (url.pathname === "/api/wireless/scan") return response({
      networks: [{ ssid: "browser-network", ssid_hex: "62726f777365722d6e6574776f726b", secured: true, rssi: -40 }]
    });
    if (url.pathname === "/api/wireless/cancel" && ++cancelAttempts === 1) {
      return response({ error: "temporary failure" }, 409);
    }
    if (url.pathname === "/api/wireless/cancel") return response({});
    return response({ error: "Unexpected browser-check request" }, 404);
  };
</script>
<script type="module" src="/js/app.mjs"></script>`;
const html = source
  .replace("<head>", '<head><base href="/">')
  .replace('<script type="module" src="/js/app.mjs"></script>', mock);
dashboard.style.width = "1000px";
dashboard.style.height = "800px";
dashboard.srcdoc = html;
await new Promise((resolve, reject) => {
  dashboard.addEventListener("load", resolve, { once: true });
  dashboard.addEventListener("error", reject, { once: true });
});
const page = dashboard.contentDocument;
await waitFor(() => page.querySelector("#connection-label").textContent === "Connected",
  "mock board did not connect");

await run("confirmation dialog focuses, closes with Escape, and restores focus", async () => {
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
  dashboard.style.width = "360px";
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
  Object.defineProperty(dashboard.contentWindow.navigator, "clipboard", { configurable: true, value: clipboard });
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

results.dataset.done = "true";
