import { renderStatus as renderDashboardStatus, renderSelfTest, storageSpeed } from "./dashboard.mjs";
import { bindWirelessForm } from "./wireless-form.mjs?v=15";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const mscStatusRefresh = document.querySelector("#msc-status-refresh");
const boardSelect = document.querySelector("#board-select");
const rescanBoardsButton = document.querySelector("#rescan-boards");
const settingsSave = document.querySelector("#settings-save");
const configurationDialog = document.querySelector("#configuration-dialog");
const rebootDialog = document.querySelector("#reboot-dialog");
const returnDialog = document.querySelector("#return-dialog");
const shareDialog = document.querySelector("#share-dialog");
let configurationForceSave = false;
const clockControl = document.querySelector("#settings-clock");
const storageControl = document.querySelector("#settings-storage");
const wifiControl = document.querySelector("#settings-wifi");
const bleControl = document.querySelector("#settings-ble");
const settingsControls = [clockControl, storageControl, wifiControl, bleControl];
const fetchWithDeadline = (url, options, timeoutMs) =>
  fetch(url, { ...options, signal: AbortSignal.timeout(timeoutMs) });
let requestInProgress = false;
let currentSnapshot = null;
let selfTestPendingSequence = null;
let selfTestRequestInProgress = false;
let ledRequestInProgress = false;
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
let wirelessLoadedAt = 0;
let wirelessOutcomeUncertain = false;
let wirelessProgress = null;
let wirelessStatus = null;
let wirelessError = null;
let wirelessReading = false;
let statusReadError = null;
let commandOutcomeUncertain = false;
let uncertainSettings = null;
let uncertainSelfTestSequence = null;
let uncertainLedState = null;
let ledError = null;
function hasUnknownOutcome(error) {
  return ["AbortError", "TimeoutError", "UnknownOutcomeError"].includes(error?.name)
    || error instanceof TypeError;
}
function markCommandOutcomeUncertain(error, message =
  "Command outcome is uncertain; waiting for the board to reconnect before retrying.") {
  if (!hasUnknownOutcome(error)) return false;
  commandOutcomeUncertain = true;
  if (currentSnapshot) {
    currentSnapshot.outcome_uncertain = true;
    currentSnapshot.error = message;
  }
  return true;
}
function endpointError(response, message) {
  const error = new Error(message);
  error.status = response.status;
  if ((response.status >= 500 && !/not supported/i.test(message))
    || /uncertain|outcome.*not resent/i.test(message)) {
    error.name = "UnknownOutcomeError";
  }
  return error;
}
async function ensureResponseOk(response, fallback) {
  if (response.ok) return;
  await responseJson(response, fallback);
}
async function responseJson(response, fallback) {
  let result;
  try { result = await response.json(); }
  catch (error) {
    if (response.ok || !(error instanceof SyntaxError)) throw error;
    result = {};
  }
  if (!response.ok) {
    throw endpointError(response, result?.error ?? result?.title ?? result?.detail ?? fallback);
  }
  return result;
}
function storageCommandPending(snapshot = currentSnapshot) {
  return mscRequestInProgress || mscPendingEnabled !== null || selfTestRequestInProgress
    || selfTestPendingSequence !== null || Boolean(snapshot?.msc?.transition_pending
      || snapshot?.self_test?.active || snapshot?.self_test_command_pending);
}
function resetBoardCommandState() {
  wirelessOutcomeUncertain = false;
  commandOutcomeUncertain = false;
  uncertainSettings = null;
  uncertainSelfTestSequence = null;
  uncertainLedState = null;
  mscError = null;
  ledError = null;
  selfTestPendingSequence = null;
  mscPendingEnabled = null;
}
async function readWirelessStatus() {
  const response = await fetchWithDeadline(boardUrl("/api/wireless"), { cache: "no-store" }, 5000);
  const result = await responseJson(response, `Wireless endpoint returned HTTP ${response.status}`);
  if (!result.wireless || typeof result.wireless.ssid !== "string") {
    throw new Error("Wireless endpoint returned invalid status");
  }
  return result.wireless;
}
async function refreshWirelessStatus() {
  const version = selectionVersion;
  wirelessReading = true;
  wirelessLoadedAt = Date.now();
  try {
    const fresh = await readWirelessStatus();
    if (version !== selectionVersion || !currentSnapshot?.connected) return;
    wirelessStatus = fresh;
    wirelessError = null;
  } catch (error) {
    if (version !== selectionVersion || !currentSnapshot?.connected) return;
    wirelessStatus = null;
    wirelessError = `Wi-Fi status read failed: ${error.message}`;
  } finally {
    wirelessReading = false;
    if (version === selectionVersion && currentSnapshot?.connected) renderStatus(currentSnapshot);
  }
}
const wirelessForm = bindWirelessForm({
  supported: () => Boolean(currentSnapshot?.hardware?.capabilities & 128),
  ready: wirelessReady,
  load: async () => wirelessStatus ?? await readWirelessStatus(),
  scan: async () => {
    const response = await fetchWithDeadline(boardUrl("/api/wireless/scan"), { method: "POST" }, 10000);
    const result = await responseJson(response, `Wireless scan returned HTTP ${response.status}`);
    return result.networks;
  },
  test: async (settings, onProgress) => {
    const version = selectionVersion;
    wirelessProgress = onProgress;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/test"), {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
      }, 40000);
      const result = await responseJson(response, `Connection test returned HTTP ${response.status}`);
      return result.test;
    } catch (error) {
      if (version === selectionVersion && hasUnknownOutcome(error)) wirelessOutcomeUncertain = true;
      if (error instanceof TypeError || ["TimeoutError", "AbortError"].includes(error?.name)) {
        throw new Error(error.name === "TimeoutError"
          ? "Wi-Fi test timed out; result is uncertain. Restart the dashboard and reconnect the board before retrying."
          : "Wi-Fi test result was lost. Restart the dashboard and reconnect the board before retrying.");
      }
      throw error;
    } finally {
      wirelessProgress = null;
    }
  },
  cancelTest: async () => {
    const response = await fetchWithDeadline(boardUrl("/api/wireless/cancel"), { method: "POST" }, 10000);
    try { return await responseJson(response, `Cancel returned HTTP ${response.status}`); }
    catch (error) {
      if (hasUnknownOutcome(error)) wirelessOutcomeUncertain = true;
      throw error;
    }
  },
  changed: () => { if (currentSnapshot) renderStatus(currentSnapshot); },
});

function wirelessReady() {
  const snapshot = currentSnapshot;
  return snapshot?.connected && Boolean(snapshot.hardware?.capabilities & 128)
    && statusReadError === null && !commandOutcomeUncertain
    && !settingsRequestInProgress && !wirelessForm.inFlight() && !snapshot.busy && !snapshot.reboot_pending
    && !snapshot.outcome_uncertain && !snapshot.command_outcome_uncertain && !wirelessOutcomeUncertain
    && !mscRequestInProgress && mscPendingEnabled === null && snapshot.storage_stale === false
    && snapshot.msc && !snapshot.msc.enabled && !snapshot.msc.transition_pending
    && !selfTestRequestInProgress && selfTestPendingSequence === null
    && !snapshot.self_test_command_pending && !snapshot.self_test?.active
    && !["pending", "busy"].includes(snapshot.control_result?.status)
    && (!(snapshot.hardware.capabilities & 32) || (snapshot.self_test && snapshot.self_test_stale === false));
}

/** Save board settings and credentials in one firmware commit; failures before delivery stay retryable. */
async function saveCombinedSettings(settings) {
  const request = { ...wirelessForm.request(), wifi_enabled: Boolean(settings.radios & 1),
    ble_enabled: Boolean(settings.radios & 2), board: settings };
  try {
    const response = await fetchWithDeadline(boardUrl("/api/wireless"), {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request),
    }, 15000);
    const result = await responseJson(response, `Wireless endpoint returned HTTP ${response.status}`);
    const saved = result.wireless;
    if (result.reboot_pending !== true || saved?.ssid !== request.ssid
      || saved.wifi_enabled !== request.wifi_enabled || saved.ble_enabled !== request.ble_enabled
      || (request.password !== undefined && saved.password_configured !== Boolean(request.password))
      || (request.tcp_cli_enabled !== undefined && saved.tcp_cli_enabled !== request.tcp_cli_enabled)
      || (request.tcp_cli_auth_required !== undefined
        && saved.tcp_cli_auth_required !== request.tcp_cli_auth_required)
      || (request.ble_key !== undefined && saved.ble_key_configured !== Boolean(request.ble_key))
      || result.settings?.system_clock_mhz !== settings.system_clock_mhz
      || result.settings?.sdcard_profile !== settings.sdcard_profile || result.settings?.radios !== settings.radios) {
      throw new Error("Wireless readback did not match the requested values");
    }
    wirelessForm.finish(saved);
    currentSnapshot.reboot_pending = true;
    return result.settings;
  } catch (error) {
    const rejected = error.status >= 400 && error.status < 500 && error.name !== "UnknownOutcomeError";
    if (!rejected) {
      wirelessOutcomeUncertain = true;
      if (currentSnapshot) currentSnapshot.outcome_uncertain = true;
      error = new Error("Wireless provisioning outcome is uncertain; command was not resent. Reload before retrying.");
    }
    wirelessForm.finish(null, error.message);
    throw error;
  }
}

function boardUrl(path, boardId = selectedBoard) {
  if (!boardId) return path;
  return `${path}${path.includes("?") ? "&" : "?"}board=${encodeURIComponent(boardId)}`;
}

function boardLabel(board) {
  return board.serial_number ? `PicoForge / ${board.serial_number}` : `PicoForge / HID ${board.device_path}`;
}

function boardSelectionBusy() {
  return scanningBoards || settingsRequestInProgress || wirelessForm.operationBusy();
}

function updateBoardSelectionControls() {
  boardSelect.disabled = boardSelectionBusy() || knownBoards.length === 0;
  rescanBoardsButton.disabled = boardSelectionBusy();
}

function renderStatus(snapshot) {
  if (!snapshot.connected) {
    shareDialog.close();
    wirelessLoadedAt = 0;
    wirelessStatus = null;
    wirelessError = null;
    wirelessForm.reset();
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = null;
  }
  updateBoardSelectionControls();
  renderDashboardStatus({
    ...snapshot,
    wireless: wirelessStatus,
    wireless_error: wirelessError,
    console_url: `/console?board=${encodeURIComponent(selectedBoard ?? "")}`,
    busy: snapshot.busy || settingsRequestInProgress || wirelessForm.operationBusy()
      || snapshot.reboot_pending || snapshot.outcome_uncertain || statusReadError !== null,
    no_board_selected: !selectedBoard,
    settings_busy: settingsRequestInProgress || wirelessForm.inFlight() || statusReadError !== null
      || snapshot.reboot_pending || snapshot.outcome_uncertain,
    settings_dirty: settingsDirty,
    wireless_dirty: wirelessForm.dirty(),
    wireless_stage_valid: wirelessForm.stageValid(),
    settings_storage_dirty: settingsStorageDirty,
    settings_message: settingsMessage ?? snapshot.settings_message,
    msc_command_pending: mscRequestInProgress || mscPendingEnabled !== null,
    msc_outcome_uncertain: mscError !== null && mscPendingEnabled !== null,
    status_read_error: statusReadError !== null,
    status_reading: requestInProgress,
  });
  wirelessForm.update();
  if (wirelessReady() && wirelessStatus) {
    void wirelessForm.load();
  }
  if (snapshot.connected && (snapshot.hardware?.capabilities & 128) && !wirelessReading
    && !wirelessForm.inFlight() && !snapshot.reboot_pending && Date.now() - wirelessLoadedAt >= 2000) {
    void refreshWirelessStatus();
  }
}

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress || settingsRequestInProgress) return;
  requestInProgress = true;
  const version = selectionVersion;
  try {
    const response = await fetchWithDeadline(boardUrl("/api/status"), { cache: "no-store" }, 5000);
    const snapshot = await responseJson(response, `Status endpoint returned HTTP ${response.status}`);
    if (version !== selectionVersion) return;
    if (mscRequestInProgress || settingsRequestInProgress) return;
    statusReadError = null;
    if ((!snapshot.connected || snapshot.reboot_pending) && !uncertainSettings
      && uncertainSelfTestSequence === null && uncertainLedState === null) {
      commandOutcomeUncertain = false;
    }
    if (uncertainLedState !== null && snapshot.board_status?.led_on != null) {
      if (Boolean(snapshot.board_status.led_on) === uncertainLedState) {
        uncertainLedState = null;
        ledError = null;
        commandOutcomeUncertain = false;
      } else commandOutcomeUncertain = true;
    }
    if (uncertainSettings && snapshot.settings?.system_clock_mhz === uncertainSettings.system_clock_mhz
      && snapshot.settings?.sdcard_profile === uncertainSettings.sdcard_profile
      && snapshot.settings?.radios === uncertainSettings.radios) {
      commandOutcomeUncertain = false;
      uncertainSettings = null;
      settingsDirty = false;
      settingsStorageDirty = false;
      settingsMessage = "Settings readback matches the requested values.";
    }
    if (uncertainSelfTestSequence !== null && Number.isInteger(snapshot.self_test?.sequence)
      && snapshot.self_test.sequence !== uncertainSelfTestSequence) {
      commandOutcomeUncertain = false;
      uncertainSelfTestSequence = null;
    }
    currentSnapshot = snapshot;
    currentSnapshot.outcome_uncertain ||= wirelessOutcomeUncertain || commandOutcomeUncertain
      || currentSnapshot.command_outcome_uncertain;
    if (currentSnapshot.connected && currentSnapshot.msc && !currentSnapshot.msc.transition_pending
      && (currentSnapshot.msc.enabled === mscPendingEnabled || currentSnapshot.msc.request_rejected)) {
      mscPendingEnabled = null;
      mscError = null;
    }
    currentSnapshot.error = mscError ?? ledError ?? currentSnapshot.error;
    if (!selfTestRequestInProgress && Number.isInteger(currentSnapshot.self_test?.sequence)
      && currentSnapshot.self_test.sequence !== selfTestPendingSequence) {
      selfTestPendingSequence = null;
    }
    currentSnapshot.self_test_command_pending = selfTestPendingSequence !== null;
    if (currentSnapshot.wireless_phase) wirelessProgress?.(currentSnapshot.wireless_phase);
    renderStatus(currentSnapshot);
  } catch (error) {
    if (version !== selectionVersion) return;
    statusReadError = error.message;
    const failedSnapshot = currentSnapshot?.connected
      ? { ...currentSnapshot, error: `Status unavailable: ${error.message}`,
        telemetry_stale: true, storage_stale: true }
      : { connected: false, error: error.message };
    renderStatus(failedSnapshot);
  } finally {
    requestInProgress = false;
    mscStatusRefresh.disabled = statusReadError !== null;
    updateBoardSelectionControls();
    const wirelessBusy = wirelessForm.operationBusy();
    ledControl.disabled = settingsRequestInProgress || wirelessBusy || currentSnapshot?.reboot_pending ||
      currentSnapshot?.outcome_uncertain || !currentSnapshot?.connected ||
      !currentSnapshot?.board_status?.led_available || ledRequestInProgress || statusReadError !== null;
    mscControl.disabled = settingsRequestInProgress || wirelessBusy || currentSnapshot?.reboot_pending
      || storageCommandPending() || currentSnapshot?.outcome_uncertain || !currentSnapshot?.connected
      || !currentSnapshot?.msc || statusReadError !== null
      || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
    if (version !== selectionVersion) void updateStatus();
  }
}

function selectBoard(boardId) {
  if (boardSelectionBusy()) {
    boardSelect.value = selectedBoard || "";
    return;
  }
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
  resetBoardCommandState();
  renderStatus({ connected: false });
  void updateStatus();
}

async function rescanBoards() {
  if (boardSelectionBusy()) return;
  scanningBoards = true;
  updateBoardSelectionControls();
  try {
    const response = await fetchWithDeadline("/api/boards", { cache: "no-store" }, 10000);
    const result = await responseJson(response, `Board scan returned HTTP ${response.status}`);
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
      resetBoardCommandState();
      renderStatus({ connected: false });
    }
    await updateStatus();
  } catch (error) {
    const message = `Board scan failed: ${error.message}`;
    if (currentSnapshot?.connected) {
      currentSnapshot = { ...currentSnapshot, error: message };
      renderStatus(currentSnapshot);
    } else {
      renderStatus({ connected: false, error: message });
    }
  } finally {
    scanningBoards = false;
    updateBoardSelectionControls();
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
  if (!currentSnapshot?.msc || storageCommandPending() || settingsRequestInProgress) return;
  const force = currentSnapshot.msc.enabled;
  if ((confirmReturn && !force) || (confirmShare && force)) return;
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
  const version = selectionVersion;
  mscRequestInProgress = true;
  mscPendingEnabled = !force;
  mscError = null;
  renderStatus(currentSnapshot);
  mscControl.disabled = true;
  document.querySelector("#msc-note").textContent = force ? "Revoking host access and attempting remount..."
    : "Preparing storage for USB...";
  document.querySelector("#msc-note").hidden = false;
  try {
    const response = await fetchWithDeadline(boardUrl(force ? "/api/msc/force-return" : "/api/msc"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(force ? { confirm_data_loss: true } : { enabled: !currentSnapshot.msc.enabled }),
    }, 15000);
    if (version !== selectionVersion) return;
    const result = await responseJson(response, `MSC endpoint returned HTTP ${response.status}`);
    if (version !== selectionVersion) return;
    const msc = result.msc ?? result;
    if (typeof msc.enabled !== "boolean") throw new Error("MSC endpoint returned invalid ownership status");
    if (version === selectionVersion && currentSnapshot) {
      currentSnapshot.msc = msc;
      currentSnapshot.error = null;
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (version === selectionVersion) {
      const uncertain = hasUnknownOutcome(error);
      if (!uncertain) mscPendingEnabled = null;
      mscError = uncertain
        ? "Storage command outcome is uncertain; waiting for updated ownership status before retrying."
        : error.message;
      if (currentSnapshot) {
        currentSnapshot.error = mscError;
        renderStatus(currentSnapshot);
      }
    }
  } finally {
    mscRequestInProgress = false;
    if (version === selectionVersion) {
      if (currentSnapshot) renderStatus(currentSnapshot);
      mscControl.disabled = !currentSnapshot?.connected || !currentSnapshot?.msc
        || mscPendingEnabled !== null || Boolean(currentSnapshot.msc.transition_pending)
        || Boolean(currentSnapshot.self_test?.active)
        || selfTestPendingSequence !== null
        || Boolean(currentSnapshot?.msc?.enabled && !currentSnapshot.msc.force_return_supported);
      if (mscPendingEnabled !== null) void updateStatus();
    } else void updateStatus();
  }
}

async function setLed() {
  if (!currentSnapshot?.board_status?.led_available || settingsRequestInProgress || ledRequestInProgress
    || statusReadError !== null || commandOutcomeUncertain) return;
  const version = selectionVersion;
  const requestedState = !currentSnapshot.board_status.led_on;
  ledRequestInProgress = true;
  ledControl.disabled = true;
  try {
    const response = await fetchWithDeadline(boardUrl("/api/led"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ on: requestedState }),
    }, 10000);
    if (version !== selectionVersion) return;
    const result = await responseJson(response, `LED endpoint returned HTTP ${response.status}`);
    if (version === selectionVersion && currentSnapshot) {
      uncertainLedState = null;
      ledError = null;
      currentSnapshot.board_status = result.board_status;
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (version === selectionVersion && currentSnapshot) {
      const message = /uncertain|outcome.*not resent/i.test(error.message)
        ? error.message : "LED command outcome is uncertain; refreshing board status.";
      if (markCommandOutcomeUncertain(error, message)) {
        uncertainLedState = requestedState;
        ledError = currentSnapshot.error;
      } else currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    ledRequestInProgress = false;
    if (version === selectionVersion) {
      ledControl.disabled = settingsRequestInProgress || wirelessForm.operationBusy()
        || currentSnapshot?.reboot_pending || currentSnapshot?.outcome_uncertain
        || !currentSnapshot?.connected || !currentSnapshot?.board_status?.led_available;
      if (commandOutcomeUncertain) void updateStatus();
    } else void updateStatus();
  }
}

function selectedSettings() {
  const saved = currentSnapshot?.settings;
  return {
    system_clock_mhz: Number(clockControl.value),
    sdcard_profile: settingsStorageDirty ? Number(storageControl.value) : (saved?.sdcard_profile ?? 0),
    radios: selectedRadios(),
  };
}

function selectedRadios() {
  return currentSnapshot?.hardware?.capabilities & 128
    ? Number(wifiControl.value) | Number(bleControl.value) << 1 : 0;
}

function updateSettingsDirty() {
  const saved = currentSnapshot?.settings;
  if (!saved) return;
  settingsStorageDirty = Number(storageControl.value) !== storageSpeed(saved);
  settingsDirty = Number(clockControl.value) !== saved.system_clock_mhz || settingsStorageDirty
    || selectedRadios() !== (saved.radios ?? 0);
  settingsMessage = null;
  renderStatus(currentSnapshot);
}

async function saveSettings() {
  if (!configurationDialog.open) return;
  configurationDialog.close();
  const msc = currentSnapshot?.msc;
  const force = configurationForceSave;
  if (!currentSnapshot?.connected || !currentSnapshot.settings_supported
      || !(settingsDirty || wirelessForm.dirty())
      || wirelessForm.operationBusy() || !wirelessForm.canSave()
      || settingsRequestInProgress || storageCommandPending() || !msc
      || msc.enabled !== force || (force && !(currentSnapshot.hardware?.capabilities & 64))
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)) return;
  const combined = wirelessForm.dirty();
  if (combined && (force || !wirelessReady())) return;
  settingsRequestInProgress = true;
  settingsMessage = "Saving boot settings...";
  const settings = selectedSettings();
  renderStatus(currentSnapshot);
  try {
    if (combined) {
      currentSnapshot.settings = await saveCombinedSettings(settings);
      settingsDirty = false;
      settingsStorageDirty = false;
      settingsMessage = "Settings saved. Reboot requested. Waiting for the device to reconnect.";
      return;
    }
    const response = await fetchWithDeadline(boardUrl("/api/settings"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(force ? { ...settings, confirm_data_loss: true } : settings),
    }, 15000);
    const result = await responseJson(response, `Settings endpoint returned HTTP ${response.status}`);
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
    if (markCommandOutcomeUncertain(error)) {
      uncertainSettings = settings;
      settingsMessage = "Save outcome is uncertain; checking status before allowing another save.";
    } else settingsMessage = `Save failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    updateBoardSelectionControls();
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (commandOutcomeUncertain) void updateStatus();
  }
}

async function requestReboot() {
  const response = await fetchWithDeadline(boardUrl("/api/reboot"), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ confirm: true }),
  }, 15000);
  await ensureResponseOk(response, `Reboot endpoint returned HTTP ${response.status}`);
}

async function rebootDevice() {
  if (!rebootDialog.open) return;
  rebootDialog.close();
  const msc = currentSnapshot?.msc;
  if (!currentSnapshot?.connected || settingsRequestInProgress || storageCommandPending()
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)
      || !msc || msc.enabled) return;
  settingsRequestInProgress = true;
  settingsMessage = "Requesting reboot...";
  renderStatus(currentSnapshot);
  try {
    await requestReboot();
    settingsMessage = "Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    if (markCommandOutcomeUncertain(error)) {
      settingsMessage = /uncertain|outcome.*not resent/i.test(error.message)
        ? error.message : "Reboot outcome is uncertain; waiting for the board to reconnect before retrying.";
    } else settingsMessage = `Reboot failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (commandOutcomeUncertain) void updateStatus();
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
    const response = await fetchWithDeadline(boardUrl("/api/self-test"), {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action, size_mib: document.querySelector("#self-test-size").valueAsNumber }),
    }, 10000);
    await ensureResponseOk(response, `Self-test endpoint returned HTTP ${response.status}`);
    if (version !== selectionVersion) return;
    selfTestPendingSequence = currentSnapshot?.self_test?.sequence ?? selfTestPendingSequence;
  } catch (error) {
    if (version !== selectionVersion) return;
    const uncertain = markCommandOutcomeUncertain(error);
    if (uncertain) uncertainSelfTestSequence = selfTestPendingSequence;
    else selfTestPendingSequence = null;
    if (currentSnapshot) {
      currentSnapshot.self_test_command_pending = uncertain;
      currentSnapshot.error = uncertain
        ? /uncertain|outcome.*not resent/i.test(error.message) ? error.message
          : "Benchmark command outcome is uncertain; checking for a new test sequence."
        : error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    selfTestRequestInProgress = false;
    if (commandOutcomeUncertain) void updateStatus();
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
  document.querySelector("#configuration-wireless-warning").hidden = !wirelessForm.dirty();
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
  document.querySelector("#reboot-consequence").textContent = settingsDirty || wirelessForm.dirty()
    ? "Unsaved dashboard changes will be lost." : "Saved next-boot settings will be applied.";
  rebootDialog.showModal();
});
document.querySelector("#cancel-configuration").addEventListener("click", () => configurationDialog.close());
document.querySelector("#cancel-reboot").addEventListener("click", () => rebootDialog.close());
document.querySelector("#cancel-return").addEventListener("click", () => returnDialog.close());
document.querySelector("#cancel-share").addEventListener("click", () => shareDialog.close());
document.querySelector("#confirm-share").addEventListener("click", () => setMsc(false, true));
document.querySelector("#confirm-return").addEventListener("click", () => setMsc(true));
mscStatusRefresh.addEventListener("click", updateStatus);
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
const pollWhenVisible = () => {
  return document.hidden ? Promise.resolve() : updateStatus();
};
document.addEventListener?.("visibilitychange", pollWhenVisible);
window.setInterval(pollWhenVisible, 1000);