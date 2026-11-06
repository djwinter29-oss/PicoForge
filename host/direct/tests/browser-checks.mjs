import { bindWirelessForm } from "../web/js/ui/sections/wireless-form.mjs";

const results = document.querySelector("#results");
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
  const deadline = Date.now() + 2000;
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

const dashboard = document.querySelector("#dashboard");
dashboard.style.width = "1000px";
dashboard.style.height = "800px";
dashboard.src = "../web/index.html";
await new Promise((resolve, reject) => {
  dashboard.addEventListener("load", resolve, { once: true });
  dashboard.addEventListener("error", reject, { once: true });
});
const page = dashboard.contentDocument;

await run("connection dialog opens with focus and Escape closes it", async () => {
  const opener = page.querySelector("#usb-open");
  const dialog = page.querySelector("#usb-connection-dialog");
  opener.focus();
  opener.click();
  check(dialog.open, "USB dialog did not open");
  check(dialog.contains(page.activeElement), "focus did not move into the dialog");
  const escape = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
  page.activeElement.dispatchEvent(escape);
  check(escape.defaultPrevented && !dialog.open, "Escape did not close the dialog");
  check(page.activeElement === opener, "focus was not restored to the opener");
});

await run("dashboard fits a narrow viewport without horizontal overflow", async () => {
  dashboard.style.width = "360px";
  dashboard.style.height = "800px";
  await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  check(page.documentElement.scrollWidth <= page.documentElement.clientWidth, "page overflows horizontally");
});

await run("invalid Bluetooth key stays visible while disconnected", async () => {
  const api = page.defaultView.navigator;
  const mocked = !api.bluetooth;
  if (mocked) Object.defineProperty(api, "bluetooth", { configurable: true, value: {
    requestDevice: async () => { throw new Error("Invalid input must not open the device chooser"); },
  } });
  try {
    page.querySelector("#bluetooth-open").click();
    const dialog = page.querySelector("#bluetooth-connection-dialog");
    page.querySelector("#direct-ble-control-key").value = "invalid";
    const scan = page.querySelector("#bluetooth-scan");
    await waitFor(() => !scan.disabled, "Bluetooth form did not become ready");
    scan.click();
    await waitFor(() => page.querySelector("#bluetooth-connection-status").textContent.includes("Invalid Control key"),
      "invalid-key error disappeared");
    check(dialog.open, "validation closed the Bluetooth dialog");
    page.querySelector("#bluetooth-connection-close").click();
  } finally { if (mocked) delete api.bluetooth; }
});

await run("Wi-Fi selection and cancellation failure/retry keep accurate state", async () => {
  let cancelAttempts = 0;
  const form = bindWirelessForm({
    scan: async () => [
      { ssid: "First", ssid_hex: "4669727374", secured: false, rssi: -40 },
      { ssid: "Second", ssid_hex: "5365636f6e64", secured: false, rssi: -50 },
    ],
    test: async () => ({ remaining_seconds: 600, ipv4: "192.0.2.2" }),
    cancelTest: async () => {
      if (++cancelAttempts === 1) throw new Error("temporary failure");
    },
    ready: () => true,
    supported: () => true,
  });
  form.update({ wireless: {
    ssid: "", wifi_enabled: false, password_configured: false, ble_enabled: false,
    ble_key_configured: false, tcp_cli_enabled: true,
  } });

  const wifi = document.querySelector("#direct-settings-wifi");
  const dialog = document.querySelector("#direct-wifi-dialog");
  wifi.focus();
  wifi.value = "configure";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  check(dialog.open, "Wi-Fi dialog did not open");
  check(dialog.contains(document.activeElement), "focus did not move into the Wi-Fi dialog");
  document.querySelector("#direct-wifi-scan").click();
  await waitFor(() => document.querySelector("#direct-wifi-networks").children.length === 2, "scan did not finish");
  const networks = document.querySelector("#direct-wifi-networks").children;
  networks[0].click();
  networks[1].click();
  check(networks[0].getAttribute("aria-pressed") === "false"
    && networks[1].getAttribute("aria-pressed") === "true", "selected network state is incorrect");
  check(!networks[1].hasAttribute("role"), "network choice overrides native button semantics");
  document.querySelector("#direct-wifi-other").click();
  check(networks[1].getAttribute("aria-pressed") === "false", "manual selection kept a scanned network selected");
  const ssid = document.querySelector("#direct-wifi-ssid");
  ssid.value = "browser-check";
  ssid.dispatchEvent(new Event("input", { bubbles: true }));
  document.querySelector("#direct-wifi-connect").click();
  await waitFor(() => !dialog.open, "Wi-Fi test did not finish and close the dialog");
  check(document.activeElement === wifi, "focus was not restored after Wi-Fi test");
  check(form.canSave(), "successful Wi-Fi test should allow saving");

  wifi.focus();
  wifi.value = "configure";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  const cancel = new Event("cancel", { cancelable: true });
  dialog.dispatchEvent(cancel);
  await waitFor(() => document.querySelector("#direct-wireless-summary").textContent.includes("temporary failure"),
    "failed cancellation was not reported");
  check(cancel.defaultPrevented && dialog.open, "failed keyboard cancellation closed the dialog");
  check(!form.canSave(), "saving remained enabled after cancellation failed");
  check(document.querySelector("#direct-wifi-cancel").textContent === "Retry clearing test", "retry is not offered");

  document.querySelector("#direct-wifi-cancel").click();
  await waitFor(() => !dialog.open, "retry did not close the dialog");
  check(cancelAttempts === 2, "cancellation retry was not attempted");
  check(form.canSave(), "saving remained blocked after cancellation succeeded");
  check(document.activeElement === wifi, "focus was not restored after cancellation retry");
  form.dispose();
});
