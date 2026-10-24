import { renderStatus as renderDashboardStatus, renderSelfTest, storageSpeed } from "./dashboard.mjs";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const boardSelect = document.querySelector("#board-select");
const rescanBoardsButton = document.querySelector("#rescan-boards");
const settingsSave = document.querySelector("#settings-save");
const settingsControls = ["#settings-clock", "#settings-storage"]
  .map(selector => document.querySelector(selector));
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;
let mscRequestInProgress = false;
let settingsRequestInProgress = false;
let settingsDirty = false;
let settingsStorageDirty = false;
let settingsMessage = null;
let mscError = null;
let selectedBoard = null;
let selectedBoardLabel = "";
let knownBoards = [];
let scanningBoards = false;
let selectionVersion = 0;

function boardUrl(path, boardId = selectedBoard) {
  if (!boardId) return path;
  return `${path}${path.includes("?") ? "&" : "?"}board=${encodeURIComponent(boardId)}`;
}

function boardLabel(board) {
  return board.serial_number ? `PicoForge / ${board.serial_number}` : `PicoForge / HID ${board.device_path}`;
}

function renderStatus(snapshot) {
  if (!snapshot.connected) {
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = null;
  }
  renderDashboardStatus({
    ...snapshot,
    no_board_selected: !selectedBoard,
    settings_busy: settingsRequestInProgress,
    settings_dirty: settingsDirty,
    settings_storage_dirty: settingsStorageDirty,
    settings_message: settingsMessage ?? snapshot.settings_message,
  });
}

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress || settingsRequestInProgress) return;
  requestInProgress = true;
  const version = selectionVersion;
  try {
    const response = await fetch(boardUrl("/api/status"), { cache: "no-store" });
    if (!response.ok) throw new Error(`Status endpoint returned HTTP ${response.status}`);
    const snapshot = await response.json();
    if (version !== selectionVersion) return;
    if (mscRequestInProgress) return;
    currentSnapshot = snapshot;
    currentSnapshot.error = mscError ?? currentSnapshot.error;
    if (currentSnapshot.self_test?.sequence !== selfTestPendingSequence) selfTestPendingSequence = null;
    currentSnapshot.self_test_command_pending = selfTestPendingSequence !== null;
    renderStatus(currentSnapshot);
  } catch (error) {
    if (version !== selectionVersion) return;
    renderStatus({ connected: false, error: error.message });
    currentSnapshot = null;
  } finally {
    requestInProgress = false;
    boardSelect.disabled = scanningBoards || knownBoards.length === 0
      || mscRequestInProgress || settingsRequestInProgress;
    rescanBoardsButton.disabled = scanningBoards;
    ledControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
    mscControl.disabled = mscRequestInProgress || !currentSnapshot?.connected || !currentSnapshot?.msc
      || Boolean(currentSnapshot.msc.transition_pending) || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
    if (version !== selectionVersion) void updateStatus();
  }
}

function selectBoard(boardId) {
  const nextBoard = boardId || null;
  if (nextBoard === selectedBoard) return;
  selectedBoard = nextBoard;
  const selected = knownBoards.find(board => board.id === selectedBoard);
  selectedBoardLabel = selected ? boardLabel(selected) : selectedBoardLabel;
  selectionVersion++;
  currentSnapshot = null;
  settingsDirty = false;
  settingsStorageDirty = false;
  settingsMessage = null;
  selfTestPendingSequence = null;
  renderStatus({ connected: false });
  void updateStatus();
}

async function rescanBoards() {
  if (scanningBoards) return;
  scanningBoards = true;
  boardSelect.disabled = true;
  rescanBoardsButton.disabled = true;
  try {
    const response = await fetch("/api/boards", { cache: "no-store" });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? `Board scan returned HTTP ${response.status}`);
    const previousBoard = selectedBoard;
    knownBoards = Array.isArray(result.boards) ? result.boards : [];
    boardSelect.replaceChildren();
    const placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent = knownBoards.length ? "Select a board" : "No boards found";
    boardSelect.append(placeholder);
    for (const board of knownBoards) {
      const option = document.createElement("option");
      option.value = board.id;
      option.textContent = boardLabel(board);
      boardSelect.append(option);
    }
    if (!selectedBoard && result.default_board_id) selectedBoard = result.default_board_id;
    const selected = knownBoards.find(board => board.id === selectedBoard);
    if (selected) {
      selectedBoardLabel = boardLabel(selected);
    } else if (selectedBoard) {
      const missing = document.createElement("option");
      missing.value = selectedBoard;
      missing.textContent = `${selectedBoardLabel || selectedBoard} (not found)`;
      boardSelect.append(missing);
    }
    boardSelect.value = selectedBoard || "";
    if (previousBoard !== selectedBoard) {
      selectionVersion++;
      currentSnapshot = null;
      settingsDirty = false;
      settingsStorageDirty = false;
      selfTestPendingSequence = null;
      renderStatus({ connected: false });
    }
    await updateStatus();
  } catch (error) {
    currentSnapshot = null;
    renderStatus({ connected: false, error: `Board scan failed: ${error.message}` });
  } finally {
    scanningBoards = false;
    boardSelect.disabled = knownBoards.length === 0;
    rescanBoardsButton.disabled = false;
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
    const response = await fetch(boardUrl(force ? "/api/msc/force-return" : "/api/msc"), {
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
    const response = await fetch(boardUrl("/api/led"), {
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
  const saved = currentSnapshot?.settings;
  return {
    system_clock_mhz: Number(settingsControls[0].value),
    sdcard_profile: settingsStorageDirty ? Number(settingsControls[1].value) : (saved?.sdcard_profile ?? 0),
  };
}

function updateSettingsDirty() {
  const saved = currentSnapshot?.settings;
  if (!saved) return;
  settingsStorageDirty = Number(settingsControls[1].value)
    !== storageSpeed(saved);
  settingsDirty = Number(settingsControls[0].value) !== saved.system_clock_mhz || settingsStorageDirty;
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
    const response = await fetch(boardUrl("/api/settings"), {
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
    settingsStorageDirty = false;
    settingsMessage = "Saved for the next boot. Reboot separately to apply.";
  } catch (error) {
    settingsMessage = `Save failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
  }

  async function rebootDevice() {
    const consequence = settingsDirty
      ? " Unsaved dashboard changes will be lost."
      : " Saved next-boot settings will be applied.";
    if (!currentSnapshot?.connected || settingsRequestInProgress
        || !window.confirm(`Reboot PicoForge now?${consequence} Continue?`)) return;
    settingsRequestInProgress = true;
    settingsMessage = "Requesting reboot...";
    renderStatus(currentSnapshot);
    try {
      const response = await fetch(boardUrl("/api/reboot"), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ confirm: true }),
      });
      if (!response.ok) throw new Error(`Reboot endpoint returned HTTP ${response.status}`);
      settingsMessage = "Reboot requested. Waiting for the device to reconnect.";
    } catch (error) {
      settingsMessage = `Reboot failed: ${error.message}`;
    } finally {
      settingsRequestInProgress = false;
      if (currentSnapshot) renderStatus(currentSnapshot);
    }
  }
}

async function selfTest(action) {
  if (!currentSnapshot?.connected || settingsRequestInProgress) return;
  selfTestPendingSequence = currentSnapshot.self_test?.sequence ?? 0;
  currentSnapshot.self_test_command_pending = true;
  renderStatus(currentSnapshot);
  try {
    const response = await fetch(boardUrl("/api/self-test"), {
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
boardSelect.addEventListener("change", () => selectBoard(boardSelect.value));
rescanBoardsButton.addEventListener("click", rescanBoards);
for (const control of settingsControls) control.addEventListener("change", updateSettingsDirty);
settingsSave.addEventListener("click", saveSettings);
document.querySelector("#reboot-control").addEventListener("click", rebootDevice);
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", () => setMsc());
void rescanBoards();
window.setInterval(updateStatus, 1000);