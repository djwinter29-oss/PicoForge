import { bindWirelessForm } from "../../web/js/ui/sections/wireless-form.mjs";
import { CliScreen } from "../../web/js/ui/cli-screen.mjs";
import { createConsoleRenderer } from "../../web/js/ui/console.mjs";
import { OP } from "../../web/js/protocol/constants.mjs";

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
dashboard.src = "../../web/index.html";
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
  let completeCancel;
  let notifications = 0;
  let testFails = false;
  const form = bindWirelessForm({
    scan: async () => [
      { ssid: "First", ssid_hex: "4669727374", secured: false, rssi: -40 },
      { ssid: "Second", ssid_hex: "5365636f6e64", secured: false, rssi: -50 },
    ],
    test: async () => {
      if (testFails) throw new Error("Replacement failed");
      return { remaining_seconds: 600, ipv4: "192.0.2.2" };
    },
    cancelTest: async () => {
      if (++cancelAttempts === 1) throw new Error("temporary failure");
      if (cancelAttempts === 3) await new Promise(resolve => { completeCancel = resolve; });
    },
    ready: () => true,
    supported: () => true,
    changed: () => notifications++,
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
  testFails = true;
  ssid.value = "replacement";
  ssid.dispatchEvent(new Event("input", { bubbles: true }));
  document.querySelector("#direct-wifi-connect").click();
  await waitFor(() => document.querySelector("#direct-wifi-status").textContent.includes("Replacement failed"),
    "failed replacement was not reported");
  check(!form.canSave(), "a failed replacement left the old tested pair saveable");
  check(document.querySelector("#direct-wireless-summary").textContent === form.saveBlockReason(),
    "failed staging does not show its Save blocker");
  testFails = false;
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
  wifi.value = "configure";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  ssid.value = "tested";
  ssid.dispatchEvent(new Event("input", { bubbles: true }));
  document.querySelector("#direct-wifi-connect").click();
  await waitFor(() => !form.operationBusy() && form.request()?.use_tested_wifi, "second test did not finish");
  wifi.blur();
  wifi.focus();
  wifi.value = "0";
  wifi.dispatchEvent(new Event("input", { bubbles: true }));
  await waitFor(() => Boolean(completeCancel), "cancellation did not start");
  form.reset();
  const before = notifications;
  completeCancel();
  await new Promise(resolve => setTimeout(resolve, 0));
  check(!dialog.open, "obsolete cancellation reopened the dialog");
  check(notifications === before, "obsolete cancellation changed the dashboard");
  form.dispose();
});

const withDashboardFixture = async action => {
  const html = await (await fetch("../../web/index.html")).text();
  const content = new DOMParser().parseFromString(html, "text/html");
  for (const asset of content.querySelectorAll("link[href]")) {
    asset.href = new URL(asset.getAttribute("href"), new URL("../../web/", location.href)).href;
  }
  content.querySelector("script").src = new URL("./fixture.mjs", location.href).href;
  const frame = document.createElement("iframe");
  frame.style.width = "1000px";
  frame.style.height = "800px";
  frame.srcdoc = `<!doctype html>${content.documentElement.outerHTML}`;
  document.body.append(frame);
  try {
    await waitFor(() => frame.contentWindow.directFixture, "dashboard fixture did not initialize");
    await action(frame.contentWindow.directFixture, frame.contentDocument);
  } finally {
    frame.contentWindow.directFixture?.dispose();
    frame.remove();
  }
};

await run("save completion preserves newer drafts and verified forced-save recovery", async () => {
  for (const outcome of ["resolve", "reject", "forced", "forced-new-draft"]) {
    await withDashboardFixture(async (fixture, page) => {
      let resolveSave, rejectSave;
      fixture.client.saveConfiguration = () => new Promise((resolve, reject) => {
        resolveSave = resolve;
        rejectSave = reject;
      });
      const forced = outcome.startsWith("forced");
      if (forced) fixture.patch({ msc: { enabled: true, transition_pending: false } });
      await Promise.resolve();
      const clock = page.querySelector("#settings-clock");
      clock.value = "250";
      clock.dispatchEvent(new Event("change"));
      await Promise.resolve();
      page.querySelector("#settings-save").click();
      page.querySelector("#confirm-configuration").click();
      check(resolveSave, `${outcome}: save did not start`);
      await fixture.client.disconnect();
      await Promise.resolve();
      await fixture.ready();
      if (forced) fixture.patch({ recovered_opcode: OP.SAVE_SETTINGS });
      if (outcome !== "forced") page.querySelector("#direct-ble-generate").click();
      await Promise.resolve();
      const key = page.querySelector("#direct-ble-secret").value;
      if (outcome === "reject") rejectSave(new Error("Old save failed"));
      else resolveSave({ tcp_cli_enabled: true });
      await new Promise(resolve => setTimeout(resolve, 0));
      check(page.querySelector("#direct-ble-secret").value === key, `${outcome}: a newer key was cleared`);
      const note = page.querySelector("#settings-note").textContent;
      check(outcome === "forced" ? note.includes("confirmed after reconnect")
        : !/Settings saved|Old save failed/.test(note), `${outcome}: obsolete save changed the UI`);
    });
  }
});

await run("failed replacement-key saves expose a blocker and allow regeneration or disabling", async () => {
  for (const recovery of ["generate", "disable"]) {
    await withDashboardFixture(async (fixture, page) => {
      fixture.client.saveConfiguration = async () => { throw new Error("Replacement save failed"); };
      page.querySelector("#direct-ble-generate").click();
      await Promise.resolve();
      const save = page.querySelector("#settings-save");
      check(!save.disabled, "replacement key did not enable Save");
      save.click();
      page.querySelector("#confirm-configuration").click();
      await waitFor(() => page.querySelector("#settings-note").textContent.includes("Replacement save failed"),
        "save failure was not shown");
      check(save.disabled && save.title.includes("replacement Control key"), "Save has no key-regeneration reason");
      check(page.querySelector("#direct-wireless-summary").textContent === save.title,
        "visible feedback and Save reason disagree");
      check(page.querySelector("#direct-ble-secret").value === "", "failed save retained the submitted key");
      if (recovery === "generate") page.querySelector("#direct-ble-generate").click();
      else {
        const radio = page.querySelector("#direct-settings-ble");
        radio.focus();
        radio.value = "0";
        radio.dispatchEvent(new Event("input"));
        radio.dispatchEvent(new Event("change"));
        const clock = page.querySelector("#settings-clock");
        clock.value = "250";
        clock.dispatchEvent(new Event("change"));
      }
      await Promise.resolve();
      check(!save.disabled, `${recovery} did not release the Save blocker`);
      check(!page.querySelector("#direct-wireless-summary").textContent, "obsolete Save blocker remains visible");
    });
  }
});

await run("telemetry bursts coalesce and avoid unchanged connection-label writes", async () => {
  await withDashboardFixture(async (fixture, page) => {
    const times = [];
    const label = page.querySelector("#connection-label");
    let labelWrites = 0;
    const observer = new MutationObserver(records => { labelWrites += records.length; });
    observer.observe(label, { childList: true, characterData: true, subtree: true });
    const uptime = page.querySelector("#uptime");
    let updates = 0;
    const telemetryObserver = new MutationObserver(records => { updates += records.length; });
    telemetryObserver.observe(uptime, { childList: true, characterData: true, subtree: true });
    let provisioningWrites = 0;
    const provisioningObserver = new MutationObserver(records => { provisioningWrites += records.length; });
    for (const id of ["direct-wireless-summary", "direct-wifi-status", "direct-ble-status"]) {
      provisioningObserver.observe(page.querySelector(`#${id}`), { childList: true, characterData: true, subtree: true });
    }
    try {
      for (let index = 0; index < 60; index++) {
        const start = performance.now();
        for (let burst = 0; burst < 3; burst++) fixture.telemetry((index * 3 + burst) * 1000);
        await new Promise(resolve => setTimeout(resolve, 0));
        if (index >= 10) times.push(performance.now() - start);
      }
      check(updates === 60, `180 telemetry events caused ${updates} uptime writes rather than 60`);
      check(labelWrites === 0, "unchanged connection labels were rewritten");
      check(provisioningWrites === 0, "unchanged provisioning feedback was rewritten");
      times.sort((a, b) => a - b);
      results.dataset.dashboardMedianMs = times[Math.floor(times.length / 2)].toFixed(2);
      results.dataset.dashboardP95Ms = times[Math.floor(times.length * 0.95)].toFixed(2);
    } finally { observer.disconnect(); telemetryObserver.disconnect(); provisioningObserver.disconnect(); }
  });
});

await run("bounded console scrollback remains correct during cursor edits", async () => {
  const screen = new CliScreen();
  screen.write(`${"x".repeat(512)}\r\n`.repeat(999));
  const terminal = document.createElement("pre");
  document.body.append(terminal);
  const render = createConsoleRenderer(terminal, document);
  const nodes = [...terminal.childNodes];
  const times = [];
  try {
    for (let index = 0; index < 60; index++) {
      screen.write(`${String.fromCharCode(65 + index % 26)}\b`);
      const start = performance.now();
      render(screen);
      if (index >= 10) times.push(performance.now() - start);
    }
    check(terminal.textContent.length === 512488, "bounded scrollback or cursor content changed");
    check(nodes.every((node, index) => terminal.childNodes[index] === node), "cursor edits replaced terminal nodes");
    check(nodes[1].textContent === String.fromCharCode(65 + 59 % 26), "cursor text did not update");
    times.sort((a, b) => a - b);
    results.dataset.consoleMedianMs = times[Math.floor(times.length / 2)].toFixed(2);
    results.dataset.consoleP95Ms = times[Math.floor(times.length * 0.95)].toFixed(2);
    // Measurements are diagnostic, not hardware-independent timing assertions.
    console.info(`Console render median ${results.dataset.consoleMedianMs}ms, p95 ${results.dataset.consoleP95Ms}ms`);
    screen.write("\r<svg onload='throw new Error(\"unsafe\")'>");
    render(screen);
    check(!terminal.querySelector("svg"), "console output was rendered as HTML");
    const streamingTimes = [];
    const phaseTimes = { parse: [], text: [], dom: [], layout: [] };
    const originalDisplay = screen.display;
    const scrollHeight = Object.getOwnPropertyDescriptor(Element.prototype, "scrollHeight").get;
    let textMs = 0, layoutMs = 0;
    screen.display = function () {
      const start = performance.now();
      const result = originalDisplay.call(this);
      textMs = performance.now() - start;
      return result;
    };
    Object.defineProperty(terminal, "scrollHeight", { configurable: true, get() {
      const start = performance.now();
      const height = scrollHeight.call(this);
      layoutMs = performance.now() - start;
      return height;
    } });
    for (let index = 0; index < 60; index++) {
      const start = performance.now();
      screen.write(`\r\n${"output ".repeat(60)}`);
      const parsed = performance.now();
      textMs = 0;
      layoutMs = 0;
      render(screen);
      const end = performance.now();
      if (index >= 10) {
        streamingTimes.push(end - start);
        phaseTimes.parse.push(parsed - start);
        phaseTimes.text.push(textMs);
        phaseTimes.layout.push(layoutMs);
        phaseTimes.dom.push(Math.max(0, end - parsed - textMs - layoutMs));
      }
    }
    screen.display = originalDisplay;
    delete terminal.scrollHeight;
    for (const [phase, samples] of Object.entries(phaseTimes)) {
      check(samples.length === 50 && samples.every(Number.isFinite), `incomplete ${phase} measurements`);
      samples.sort((a, b) => a - b);
      results.dataset[`streaming${phase[0].toUpperCase()}${phase.slice(1)}MedianMs`] =
        samples[Math.floor(samples.length / 2)].toFixed(2);
      results.dataset[`streaming${phase[0].toUpperCase()}${phase.slice(1)}P95Ms`] =
        samples[Math.floor(samples.length * 0.95)].toFixed(2);
    }
    check(screen.display()[0].split("\n").length === 1000, "continuous output exceeded the row limit");
    check(nodes.every((node, index) => terminal.childNodes[index] === node), "streaming replaced terminal nodes");
    streamingTimes.sort((a, b) => a - b);
    results.dataset.streamingMedianMs = streamingTimes[Math.floor(streamingTimes.length / 2)].toFixed(2);
    results.dataset.streamingP95Ms = streamingTimes[Math.floor(streamingTimes.length * 0.95)].toFixed(2);
    screen.write("\r\n> cmd\x1b[DZ");
    render(screen);
    check(nodes[1].textContent === " ", "interactive cursor did not advance after streaming");
    check(nodes[0].data.endsWith("> cmZ"), "interactive input echo was lost after streaming");
  } finally { terminal.remove(); }
});

results.dataset.complete = "true";
