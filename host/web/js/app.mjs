import { renderStatus } from "./dashboard.mjs";

const refresh = document.querySelector("#refresh");
const ledControl = document.querySelector("#led-control");
let requestInProgress = false;
let currentSnapshot = null;

async function updateStatus() {
  if (requestInProgress) return;
  requestInProgress = true;
  refresh.disabled = true;
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`Status endpoint returned HTTP ${response.status}`);
    currentSnapshot = await response.json();
    renderStatus(currentSnapshot);
  } catch (error) {
    renderStatus({ connected: false, error: error.message });
    currentSnapshot = null;
  } finally {
    requestInProgress = false;
    refresh.disabled = false;
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
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
ledControl.addEventListener("click", setLed);
void updateStatus();
window.setInterval(updateStatus, 1000);