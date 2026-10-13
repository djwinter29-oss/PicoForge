import { renderStatus, renderSelfTest } from "./dashboard.mjs";

const refresh = document.querySelector("#refresh");
const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;

async function updateStatus() {
  if (requestInProgress) return;
  requestInProgress = true;
  refresh.disabled = true;
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`Status endpoint returned HTTP ${response.status}`);
    currentSnapshot = await response.json();
    if (currentSnapshot.self_test?.sequence !== selfTestPendingSequence) selfTestPendingSequence = null;
    currentSnapshot.self_test_command_pending = selfTestPendingSequence !== null;
    renderStatus(currentSnapshot);
  } catch (error) {
    renderStatus({ connected: false, error: error.message });
    currentSnapshot = null;
  } finally {
    requestInProgress = false;
    refresh.disabled = false;
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
    mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending) || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null;
  }
}

async function setMsc() {
  if (!currentSnapshot?.msc) return;
  mscControl.disabled = true;
  try {
    const response = await fetch("/api/msc", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ enabled: !currentSnapshot.msc.enabled }),
    });
    if (!response.ok) throw new Error(`MSC endpoint returned HTTP ${response.status}`);
    const result = await response.json();
    currentSnapshot.msc = result.msc;
    renderStatus(currentSnapshot);
  } catch (error) {
    currentSnapshot.error = error.message;
    renderStatus(currentSnapshot);
  } finally {
    mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending);
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

refresh.addEventListener("click", updateStatus);
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
    if (action === "start") document.querySelector("#self-test-consent").checked = false;
  } catch (error) {
    selfTestPendingSequence = null;
    currentSnapshot.self_test_command_pending = false;
    currentSnapshot.error = error.message;
    renderStatus(currentSnapshot);
  }
}
document.querySelector("#self-test-start").addEventListener("click", () => selfTest("start"));
document.querySelector("#self-test-cancel").addEventListener("click", () => selfTest("cancel"));
for (const selector of ["#self-test-size", "#self-test-consent"]) {
  document.querySelector(selector).addEventListener("input", () => renderSelfTest(currentSnapshot ?? {}));
}
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", setMsc);
void updateStatus();
window.setInterval(updateStatus, 1000);