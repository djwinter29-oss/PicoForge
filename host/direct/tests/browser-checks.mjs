import { bindWirelessForm } from "../js/wireless-form.mjs?v=47";

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
dashboard.src = "../index.html";
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

await run("Wi-Fi cancellation failure keeps the dialog open and retry clears the test", async () => {
  let cancelAttempts = 0;
  const form = bindWirelessForm({
    load: async () => ({
      ssid: "", wifi_enabled: false, password_configured: false, ble_enabled: false,
      ble_key_configured: false, tcp_cli_enabled: true,
    }),
    scan: async () => [],
    test: async () => ({ remaining_seconds: 600, ipv4: "192.0.2.2" }),
    cancelTest: async () => {
      if (++cancelAttempts === 1) throw new Error("temporary failure");
    },
    ready: () => true,
    supported: () => true,
  });
  await form.load();

  const wifi = document.querySelector("#settings-wifi");
  const dialog = document.querySelector("#wifi-dialog");
  wifi.focus();
  wifi.value = "configure";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  check(dialog.open, "Wi-Fi dialog did not open");
  check(dialog.contains(document.activeElement), "focus did not move into the Wi-Fi dialog");
  document.querySelector("#wifi-other").click();
  const ssid = document.querySelector("#wifi-ssid");
  ssid.value = "browser-check";
  ssid.dispatchEvent(new Event("input", { bubbles: true }));
  document.querySelector("#wifi-connect").click();
  await waitFor(() => !dialog.open, "Wi-Fi test did not finish and close the dialog");
  check(document.activeElement === wifi, "focus was not restored after Wi-Fi test");
  check(form.canSave(), "successful Wi-Fi test should allow saving");

  wifi.focus();
  wifi.value = "configure";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  const cancel = new Event("cancel", { cancelable: true });
  dialog.dispatchEvent(cancel);
  await waitFor(() => document.querySelector("#wireless-summary").textContent.includes("temporary failure"),
    "failed cancellation was not reported");
  check(cancel.defaultPrevented && dialog.open, "failed keyboard cancellation closed the dialog");
  check(!form.canSave(), "saving remained enabled after cancellation failed");
  check(document.querySelector("#wifi-cancel").textContent === "Retry clearing test", "retry is not offered");

  document.querySelector("#wifi-cancel").click();
  await waitFor(() => !dialog.open, "retry did not close the dialog");
  check(cancelAttempts === 2, "cancellation retry was not attempted");
  check(form.canSave(), "saving remained blocked after cancellation succeeded");
  check(document.activeElement === wifi, "focus was not restored after cancellation retry");
});
