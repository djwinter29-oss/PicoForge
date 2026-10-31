import { renderStatus as renderDashboardStatus, renderSelfTest, storageSpeed } from "./dashboard.mjs";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const boardSelect = document.querySelector("#board-select");
const rescanBoardsButton = document.querySelector("#rescan-boards");
const settingsSave = document.querySelector("#settings-save");
const configurationDialog = document.querySelector("#configuration-dialog");
const rebootDialog = document.querySelector("#reboot-dialog");
const returnDialog = document.querySelector("#return-dialog");
const shareDialog = document.querySelector("#share-dialog");
let configurationForceSave = false;
const settingsControls = ["#settings-clock", "#settings-storage", "#settings-wifi", "#settings-ble"]
  .map(selector => document.querySelector(selector));
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;
let selfTestRequestInProgress = false;
let mscRequestInProgress = false;
let mscPendingEnabled = null;
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
    shareDialog.close();
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = null;
  }
  renderDashboardStatus({
    ...snapshot,
    busy: snapshot.busy || settingsRequestInProgress || snapshot.reboot_pending || snapshot.outcome_uncertain,
    no_board_selected: !selectedBoard,
    settings_busy: settingsRequestInProgress || snapshot.reboot_pending || snapshot.outcome_uncertain,
    settings_dirty: settingsDirty,
    settings_storage_dirty: settingsStorageDirty,
    settings_message: settingsMessage ?? snapshot.settings_message,
    msc_command_pending: mscRequestInProgress || mscPendingEnabled !== null,
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
    if (currentSnapshot.connected && currentSnapshot.msc && !currentSnapshot.msc.transition_pending
      && (currentSnapshot.msc.enabled === mscPendingEnabled || currentSnapshot.msc.request_rejected)) {
      mscPendingEnabled = null;
    }
    currentSnapshot.error = mscError ?? currentSnapshot.error;
    if (!selfTestRequestInProgress && currentSnapshot.self_test?.sequence !== selfTestPendingSequence) {
      selfTestPendingSequence = null;
    }
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
    ledControl.disabled = settingsRequestInProgress || currentSnapshot?.reboot_pending ||
      currentSnapshot?.outcome_uncertain || !currentSnapshot?.connected ||
      !currentSnapshot?.board_status?.led_available;
    mscControl.disabled = settingsRequestInProgress || currentSnapshot?.reboot_pending ||
      currentSnapshot?.outcome_uncertain || mscRequestInProgress || !currentSnapshot?.connected || !currentSnapshot?.msc
      || mscPendingEnabled !== null || Boolean(currentSnapshot.msc.transition_pending)
      || Boolean(currentSnapshot.self_test?.active)
      || selfTestPendingSequence !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
    if (version !== selectionVersion) void updateStatus();
  }
}

function selectBoard(boardId) {
  const nextBoard = boardId || null;
  if (nextBoard === selectedBoard) return;
  configurationDialog.close();
  rebootDialog.close();
  returnDialog.close();
  shareDialog.close();
  selectedBoard = nextBoard;
  const selected = knownBoards.find(board => board.id === selectedBoard);
  selectedBoardLabel = selected ? boardLabel(selected) : selectedBoardLabel;
  selectionVersion++;
  currentSnapshot = null;
  settingsDirty = false;
  settingsStorageDirty = false;
  settingsMessage = null;
  selfTestPendingSequence = null;
  mscPendingEnabled = null;
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
      mscPendingEnabled = null;
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

async function setMsc(confirmReturn = false, confirmShare = false) {
  if (confirmShare) {
    if (!shareDialog.open) return;
    shareDialog.close();
  }
  if (confirmReturn) {
    if (!returnDialog.open) return;
    returnDialog.close();
  }
  if (!currentSnapshot?.msc || mscRequestInProgress || mscPendingEnabled !== null
    || settingsRequestInProgress) return;
  const force = currentSnapshot.msc.enabled;
  if (currentSnapshot.msc.transition_pending || currentSnapshot.self_test?.active
      || selfTestPendingSequence !== null || (confirmReturn && !force) || (confirmShare && force)) return;
  if (force) {
    if (!currentSnapshot.msc.force_return_supported) return;
    if (!confirmReturn) {
      returnDialog.showModal();
      return;
    }
  } else if (!confirmShare) {
    shareDialog.showModal();
    return;
  }
  mscRequestInProgress = true;
  mscPendingEnabled = !force;
  mscError = null;
  renderStatus(currentSnapshot);
  mscControl.disabled = true;
  document.querySelector("#msc-note").textContent = force ? "Revoking host access and attempting remount..."
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
    mscPendingEnabled = null;
    mscError = error.message;
    if (currentSnapshot) {
      currentSnapshot.error = mscError;
      renderStatus(currentSnapshot);
    }
  } finally {
    mscRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
    mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
      || mscPendingEnabled !== null || Boolean(currentSnapshot.msc.transition_pending)
      || Boolean(currentSnapshot.self_test?.active)
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
    radios: selectedRadios(),
  };
}

function selectedRadios() {
  return currentSnapshot?.hardware?.capabilities & 128
    ? Number(settingsControls[2].value) | Number(settingsControls[3].value) << 1 : 0;
}

function updateSettingsDirty() {
  const saved = currentSnapshot?.settings;
  if (!saved) return;
  settingsStorageDirty = Number(settingsControls[1].value)
    !== storageSpeed(saved);
  settingsDirty = Number(settingsControls[0].value) !== saved.system_clock_mhz || settingsStorageDirty
    || selectedRadios() !== (saved.radios ?? 0);
  settingsMessage = null;
  renderStatus(currentSnapshot);
}

async function saveSettings() {
  if (!configurationDialog.open) return;
  configurationDialog.close();
  const msc = currentSnapshot?.msc;
  const force = configurationForceSave;
  if (!currentSnapshot?.connected || !currentSnapshot.settings_supported || !settingsDirty
      || settingsRequestInProgress || mscRequestInProgress || mscPendingEnabled !== null || !msc
      || msc.enabled !== force || msc.transition_pending || (force && !(currentSnapshot.hardware?.capabilities & 64))
      || currentSnapshot.self_test?.active || currentSnapshot.self_test_command_pending
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)) return;
  settingsRequestInProgress = true;
  settingsMessage = "Saving boot settings...";
  const settings = selectedSettings();
  renderStatus(currentSnapshot);
  try {
    const response = await fetch(boardUrl("/api/settings"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(force ? { ...settings, confirm_data_loss: true } : settings),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? result.title ?? `Settings endpoint returned HTTP ${response.status}`);
    if (result.settings?.system_clock_mhz !== settings.system_clock_mhz
        || result.settings?.sdcard_profile !== settings.sdcard_profile
        || (result.settings?.radios ?? 0) !== settings.radios) {
      throw new Error("Settings readback did not match the requested values");
    }
    currentSnapshot.settings = result.settings;
    currentSnapshot.settings_supported = true;
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = "Settings saved. Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    settingsMessage = `Save failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
  }
}

async function requestReboot() {
  const response = await fetch(boardUrl("/api/reboot"), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ confirm: true }),
  });
  if (!response.ok) throw new Error(`Reboot endpoint returned HTTP ${response.status}`);
}

async function rebootDevice() {
  if (!rebootDialog.open) return;
  rebootDialog.close();
  const msc = currentSnapshot?.msc;
  if (!currentSnapshot?.connected || settingsRequestInProgress
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)
      || !msc || msc.enabled || msc.transition_pending
      || currentSnapshot.self_test?.active || currentSnapshot.self_test_command_pending) return;
  settingsRequestInProgress = true;
  settingsMessage = "Requesting reboot...";
  renderStatus(currentSnapshot);
  try {
    await requestReboot();
    settingsMessage = "Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    settingsMessage = `Reboot failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
  }
}

async function selfTest(action) {
  if (!currentSnapshot?.connected || settingsRequestInProgress || selfTestRequestInProgress
    || selfTestPendingSequence !== null) return;
  const version = selectionVersion;
  selfTestRequestInProgress = true;
  selfTestPendingSequence = currentSnapshot.self_test?.sequence ?? 0;
  currentSnapshot.self_test_command_pending = true;
  renderStatus(currentSnapshot);
  try {
    const response = await fetch(boardUrl("/api/self-test"), {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action, size_mib: document.querySelector("#self-test-size").valueAsNumber }),
    });
    if (!response.ok) throw new Error(`Self-test endpoint returned HTTP ${response.status}`);
    if (version !== selectionVersion) return;
    selfTestPendingSequence = currentSnapshot?.self_test?.sequence ?? selfTestPendingSequence;
  } catch (error) {
    if (version !== selectionVersion) return;
    selfTestPendingSequence = null;
    if (currentSnapshot) {
      currentSnapshot.self_test_command_pending = false;
      currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    selfTestRequestInProgress = false;
  }
}
document.querySelector("#self-test-start").addEventListener("click", () => selfTest("start"));
document.querySelector("#self-test-cancel").addEventListener("click", () => selfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(currentSnapshot ?? {}));
boardSelect.addEventListener("change", () => selectBoard(boardSelect.value));
rescanBoardsButton.addEventListener("click", rescanBoards);
for (const control of settingsControls) control.addEventListener("change", updateSettingsDirty);
settingsSave.addEventListener("click", () => {
  if (settingsSave.disabled) return;
  configurationForceSave = Boolean(currentSnapshot?.msc?.enabled);
  document.querySelector("#configuration-force-warning").hidden = !configurationForceSave;
  const settings = selectedSettings();
  document.querySelector("#configuration-dialog-profile").textContent =
    `System clock: ${settings.system_clock_mhz ? `${settings.system_clock_mhz} MHz` : "Default"}. `
    + `SD Card: ${settings.sdcard_profile === 1 ? "High-Speed" : "Standard"}.`
    + (currentSnapshot?.hardware?.capabilities & 128 ? ` Wi-Fi: ${settings.radios & 1 ? "Enabled" : "Disabled"}.`
      + ` Bluetooth: ${settings.radios & 2 ? "Enabled" : "Disabled"}.` : "");
  configurationDialog.showModal();
});
document.querySelector("#reboot-control").addEventListener("click", () => {
  if (document.querySelector("#reboot-control").disabled) return;
  document.querySelector("#reboot-consequence").textContent = settingsDirty
    ? "Unsaved dashboard changes will be lost." : "Saved next-boot settings will be applied.";
  rebootDialog.showModal();
});
document.querySelector("#cancel-configuration").addEventListener("click", () => configurationDialog.close());
document.querySelector("#cancel-reboot").addEventListener("click", () => rebootDialog.close());
document.querySelector("#cancel-return").addEventListener("click", () => returnDialog.close());
document.querySelector("#cancel-share").addEventListener("click", () => shareDialog.close());
document.querySelector("#confirm-share").addEventListener("click", () => setMsc(false, true));
document.querySelector("#confirm-return").addEventListener("click", () => setMsc(true));
for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) {
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault();
      dialog.close();
    }
  });
}
document.querySelector("#confirm-configuration").addEventListener("click", saveSettings);
document.querySelector("#confirm-reboot").addEventListener("click", rebootDevice);
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", () => setMsc());
void rescanBoards();
window.setInterval(updateStatus, 1000);