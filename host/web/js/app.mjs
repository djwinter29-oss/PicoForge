import { renderStatus } from "./dashboard.mjs";

const refresh = document.querySelector("#refresh");
let requestInProgress = false;

async function updateStatus() {
  if (requestInProgress) return;
  requestInProgress = true;
  refresh.disabled = true;
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw new Error(`Status endpoint returned HTTP ${response.status}`);
    renderStatus(await response.json());
  } catch (error) {
    renderStatus({ connected: false, error: error.message });
  } finally {
    requestInProgress = false;
    refresh.disabled = false;
  }
}

refresh.addEventListener("click", updateStatus);
void updateStatus();
window.setInterval(updateStatus, 1000);