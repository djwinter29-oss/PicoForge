import { renderStatus as renderDashboardStatus, renderSelfTest } from "./dashboard.mjs";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const settingsSave = document.querySelector("#settings-save");
const settingsControls = ["#settings-clock", "#settings-sdio", "#settings-spi"]
  .map(selector => document.querySelector(selector));
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;
let mscRequestInProgress = false;
let settingsRequestInProgress = false;
let settingsDirty = false;
let settingsMessage = null;
let mscError = null;

function renderStatus(snapshot) {
  if (!snapshot.connected) {
    settingsDirty = false;
    settingsMessage = null;
  }
  renderDashboardStatus({
    ...snapshot,
    settings_busy: settingsRequestInProgress,
    settings_dirty: settingsDirty,
    settings_message: settingsMessage ?? snapshot.settings_message,
  });
}

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress || settingsRequestInProgress) return;
  requestInProgress = true;
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`Status endpoint returned HTTP ${response.status}`);
    const snapshot = await response.json();
    if (mscRequestInProgress) return;
    currentSnapshot = snapshot;
    currentSnapshot.error = mscError ?? currentSnapshot.error;
    if (currentSnapshot.self_test?.sequence !== selfTestPendingSequence) selfTestPendingSequence = null;
    currentSnapshot.self_test_command_pending = selfTestPendingSequence !== null;
    renderStatus(currentSnapshot);
  } catch (error) {
    renderStatus({ connected: false, error: error.message });
    currentSnapshot = null;
  } finally {
    requestInProgress = false;
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
    mscControl.disabled = mscRequestInProgress || !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending) || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
  }
}

async function setMsc() {
  if (!currentSnapshot?.msc || mscRequestInProgress || settingsRequestInProgress) return;
  const force = currentSnapshot.msc.enabled;
  if (force && (!currentSnapshot.msc.force_return_supported
    || !window.confirm("Disable USB drive and return storage to Pico? "
      + "Cached host writes may be lost and the filesystem may be corrupted. "
      + "Stop all host file activity first. Pico will attempt to remount without formatting or repair. Continue?"))) return;
  mscRequestInProgress = true;
  mscError = null;
  mscControl.disabled = true;
  document.querySelector("#msc-note").textContent = force ? "Detaching USB drive on Pico and attempting remount..."
    : "Preparing storage for USB...";
  document.querySelector("#msc-note").hidden = false;
  try {
    const response = await fetch(force ? "/api/msc/force-return" : "/api/msc", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(force ? { confirm_data_loss: true } : { enabled: !currentSnapshot.msc.enabled }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? result.title ?? `MSC endpoint returned HTTP ${response.status}`);
    const msc = result.msc ?? result;
    if (typeof msc.enabled !== "boolean") throw new Error("MSC endpoint returned invalid ownership status");
    if (currentSnapshot) {
      currentSnapshot.msc = msc;
      currentSnapshot.error = null;
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    mscError = error.message;
    if (currentSnapshot) {
      currentSnapshot.error = mscError;
      renderStatus(currentSnapshot);
    }
  } finally {
    mscRequestInProgress = false;
    mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending) || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
  }
}

async function setLed() {
  if (!currentSnapshot?.board_status?.led_available || settingsRequestInProgress) return;
  ledControl.disabled = true;
  try {
    const response = await fetch("/api/led", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ on: !currentSnapshot.board_status.led_on }),
    });
    if (!response.ok) throw new Error(`LED endpoint returned HTTP ${response.status}`);
    const result = await response.json();
    if (currentSnapshot) {
      currentSnapshot.board_status = result.board_status;
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (currentSnapshot) {
      currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
  }
}

function selectedSettings() {
  return {
    system_clock_mhz: Number(settingsControls[0].value),
    sdio_bus_mhz: Number(settingsControls[1].value),
    spi_bus_mhz: Number(settingsControls[2].value),
  };
}

function updateSettingsDirty() {
  const saved = currentSnapshot?.settings;
  if (!saved) return;
  const selected = selectedSettings();
  settingsDirty = selected.system_clock_mhz !== saved.system_clock_mhz
    || selected.sdio_bus_mhz !== (saved.sdio_bus_mhz === 25 ? 0 : saved.sdio_bus_mhz)
    || selected.spi_bus_mhz !== saved.spi_bus_mhz;
  settingsMessage = null;
  renderStatus(currentSnapshot);
}

async function saveSettings() {
  const msc = currentSnapshot?.msc;
  if (!currentSnapshot?.connected || !currentSnapshot.settings_supported || !settingsDirty
      || settingsRequestInProgress || !msc || msc.enabled || msc.transition_pending
      || currentSnapshot.self_test?.active || currentSnapshot.self_test_command_pending) return;
  settingsRequestInProgress = true;
  settingsMessage = "Saving boot settings...";
  renderStatus(currentSnapshot);
  try {
    const response = await fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(selectedSettings()),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? result.title ?? `Settings endpoint returned HTTP ${response.status}`);
    if (!result.settings || typeof result.settings.system_clock_mhz !== "number") {
      throw new Error("Settings endpoint returned invalid readback");
    }
    currentSnapshot.settings = result.settings;
    currentSnapshot.settings_supported = true;
    settingsDirty = false;
    settingsMessage = "Saved. Changes take effect after reboot.";
  } catch (error) {
    settingsMessage = `Save failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
  }
}

async function selfTest(action) {
  if (!currentSnapshot?.connected || settingsRequestInProgress) return;
  selfTestPendingSequence = currentSnapshot.self_test?.sequence ?? 0;
  currentSnapshot.self_test_command_pending = true;
  renderStatus(currentSnapshot);
  try {
    const response = await fetch("/api/self-test", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action, size_mib: document.querySelector("#self-test-size").valueAsNumber }),
    });
    if (!response.ok) throw new Error(`Self-test endpoint returned HTTP ${response.status}`);
  } catch (error) {
    selfTestPendingSequence = null;
    if (currentSnapshot) {
      currentSnapshot.self_test_command_pending = false;
      currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  }
}
document.querySelector("#self-test-start").addEventListener("click", () => selfTest("start"));
document.querySelector("#self-test-cancel").addEventListener("click", () => selfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(currentSnapshot ?? {}));
for (const control of settingsControls) control.addEventListener("change", updateSettingsDirty);
settingsSave.addEventListener("click", saveSettings);
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", () => setMsc());
void updateStatus();
window.setInterval(updateStatus, 1000);