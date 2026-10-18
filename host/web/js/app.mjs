import { renderStatus, renderSelfTest } from "./dashboard.mjs";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;
let mscRequestInProgress = false;
let mscError = null;

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress) return;
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
  if (!currentSnapshot?.msc || mscRequestInProgress) return;
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
    currentSnapshot.msc = msc;
    currentSnapshot.error = null;
    renderStatus(currentSnapshot);
  } catch (error) {
    mscError = error.message;
    currentSnapshot.error = mscError;
    renderStatus(currentSnapshot);
  } finally {
    mscRequestInProgress = false;
    mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending) || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
  }
}

async function setLed() {
  if (!currentSnapshot?.board_status?.led_available) return;
  ledControl.disabled = true;
  try {
    const response = await fetch("/api/led", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ on: !currentSnapshot.board_status.led_on }),
    });
    if (!response.ok) throw new Error(`LED endpoint returned HTTP ${response.status}`);
    const result = await response.json();
    currentSnapshot.board_status = result.board_status;
    renderStatus(currentSnapshot);
  } catch (error) {
    currentSnapshot.error = error.message;
    renderStatus(currentSnapshot);
  } finally {
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
  }
}

async function selfTest(action) {
  if (!currentSnapshot?.connected) return;
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
    currentSnapshot.self_test_command_pending = false;
    currentSnapshot.error = error.message;
    renderStatus(currentSnapshot);
  }
}
document.querySelector("#self-test-start").addEventListener("click", () => selfTest("start"));
document.querySelector("#self-test-cancel").addEventListener("click", () => selfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(currentSnapshot ?? {}));
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", () => setMsc());
void updateStatus();
window.setInterval(updateStatus, 1000);