// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { normalizeStatus, renderStatus as renderDashboardStatus, storageSpeed } from "./dashboard.mjs";
import { bindWirelessForm } from "./wireless-form.mjs?v=19";
import {
  acknowledge, DashboardError, reconciledResult, responseJson, savedSettings, scannedNetworks,
} from "./dashboard-api.mjs";
import { commandEffects, commandSettled, operationState as deriveOperationState } from "./operation-state.mjs";

const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const mscStatusRefresh = document.querySelector("#msc-status-refresh");
const commandResync = document.querySelector("#command-resync");
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
let disconnectedUi = false;
let selfTestPendingSequence = null;
let selfTestRequestInProgress = false;
let ledRequestInProgress = false;
let commandResyncInProgress = false;
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
let boardScanWarning = null;
let unknownBoardRescanAt = 0;
let selectionVersion = 0;
let wirelessProgress = null;
let statusReadError = null;
/** @type {import("./operation-state.mjs").UncertainCommand | null} */
let uncertainCommand = null;
/**
 * Acknowledgement is not reconnection; old snapshots cannot clear this per-board lifecycle gate.
 * @type {"requested" | "observed" | "disconnected" | null}
 */
let rebootPending = null;
let ledError = null;
/** @type {{opcode: number, message: string} | null} */
let commandFailure = null;
const boardCommandStates = new Map();
function hasUnknownOutcome(error) {
  return ["AbortError", "TimeoutError", "UnknownOutcomeError"].includes(error?.name)
    || error instanceof TypeError;
}
function isUncertainOutcome(error) {
  if (typeof error?.uncertain === "boolean") return error.uncertain;
  return /uncertain|outcome.*not resent/i.test(error?.message ?? "");
}
function uncertainMessage(error, fallback) {
  return isUncertainOutcome(error) && /uncertain|outcome.*not resent/i.test(error?.message ?? "")
    ? error.message : fallback;
}
/** @param {Error} error @param {string} [message]
 * @param {import("./operation-state.mjs").Recovery} [recovery] @param {string | null} [boardId]
 */
function markCommandOutcomeUncertain(error, message =
  "Command outcome is uncertain. Use Re-sync before retrying.", recovery = { kind: "reconnect" }, boardId = selectedBoard) {
  if (!hasUnknownOutcome(error)) return false;
  const command = { ...recovery, message };
  if (boardId !== selectedBoard) {
    if (boardId) boardCommandStates.set(boardId, { ...boardCommandStates.get(boardId), uncertainCommand: command });
    return true;
  }
  uncertainCommand = command;
  saveBoardCommandState();
  if (currentSnapshot) {
    currentSnapshot.error = message;
  }
  return true;
}
function restoreBoardCommandState() {
  const saved = boardCommandStates.get(selectedBoard);
  uncertainCommand = saved?.uncertainCommand ?? null;
  mscError = saved?.mscError ?? null;
  ledError = saved?.ledError ?? null;
  selfTestPendingSequence = saved?.selfTestPendingSequence ?? null;
  mscPendingEnabled = saved?.mscPendingEnabled ?? null;
  rebootPending = saved?.rebootPending ?? null;
  commandFailure = saved?.commandFailure ?? null;
}
function saveBoardCommandState(boardId = selectedBoard) {
  if (!boardId) return;
  boardCommandStates.set(boardId, {
    uncertainCommand,
    mscError,
    ledError,
    selfTestPendingSequence,
    mscPendingEnabled,
    rebootPending,
    commandFailure,
  });
}
/** @typedef {import("./operation-state.mjs").OperationSnapshot} OperationSnapshot */
/** @typedef {import("./operation-state.mjs").OperationState} OperationState */
/** @param {OperationSnapshot | null} [snapshot] @returns {OperationState} */
function operationState(snapshot = currentSnapshot) {
  return deriveOperationState(snapshot, {
    uncertain: uncertainCommand !== null, rebootPending: rebootPending !== null,
    resyncing: commandResyncInProgress, saving: settingsRequestInProgress,
    wirelessBusy: wirelessForm.operationBusy(), wirelessInFlight: wirelessForm.inFlight(),
    statusFailed: statusReadError !== null, mscInFlight: mscRequestInProgress, mscPending: mscPendingEnabled !== null,
    selfTestInFlight: selfTestRequestInProgress, selfTestPending: selfTestPendingSequence !== null,
    ledInFlight: ledRequestInProgress, scanning: scanningBoards,
  }, document.querySelector("#self-test-size").valueAsNumber);
}

/** Apply the same confirmed lifecycle effects for ordinary acknowledgements and explicit Re-sync.
 * @param {import("./dashboard-api.mjs").ControlResult} result
 */
function publishCommandResult(result) {
  const effects = commandEffects(result);
  if (effects.releaseMsc) {
    mscPendingEnabled = null;
    mscError = null;
  }
  if (effects.releaseSelfTest) selfTestPendingSequence = null;
  if (effects.reboot) rebootPending = "requested";
  if (effects.wifiCancel) {
    if (effects.completed && currentSnapshot) currentSnapshot.wireless_cleanup_required = false;
    wirelessForm.cancelResolved(effects.completed);
  }
  if (effects.failure) commandFailure = { opcode: result.opcode, message: effects.failure };
  else clearCommandFailure(result.opcode);
  saveBoardCommandState();
}
/** @param {number} opcode */
function clearCommandFailure(opcode) {
  if (commandFailure?.opcode === opcode) commandFailure = null;
}
function storageCommandPending(snapshot = currentSnapshot) {
  return operationState(snapshot).storageCommandPending;
}
const wirelessForm = bindWirelessForm({
  supported: () => Boolean(currentSnapshot?.hardware?.capabilities & 128),
  cleanupRequired: () => Boolean(currentSnapshot?.wireless_cleanup_required),
  ready: wirelessReady,
  scan: async () => {
    const boardId = selectedBoard;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/scan", boardId), { method: "POST" }, 20000);
      const result = await responseJson(response, `Wireless scan returned HTTP ${response.status}`, true);
      return scannedNetworks(result);
    } catch (error) {
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      throw error;
    }
  },
  test: async (settings, onProgress) => {
    const version = selectionVersion;
    const boardId = selectedBoard;
    wirelessProgress = onProgress;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/test"), {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
      }, 40000);
      const result = await responseJson(response, `Connection test returned HTTP ${response.status}`, true);
      if (!Number.isInteger(result.test?.remaining_seconds) || result.test.remaining_seconds < 1
        || result.test.remaining_seconds > 600)
        throw new DashboardError("Invalid wireless test response; outcome is uncertain", undefined, true);
      return result.test;
    } catch (error) {
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      if (error instanceof TypeError || ["TimeoutError", "AbortError"].includes(error?.name)) {
        throw new Error(error.name === "TimeoutError"
          ? "Wi-Fi test timed out; result is uncertain. Use Re-sync before retrying."
          : "Wi-Fi test result was lost. Use Re-sync before retrying.");
      }
      throw error;
    } finally {
      if (version === selectionVersion) wirelessProgress = null;
    }
  },
  cancelTest: async () => {
    const boardId = selectedBoard;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/cancel"), { method: "POST" }, 10000);
      const result = await responseJson(response, `Cancel returned HTTP ${response.status}`, true);
      if (result.cancelled !== true)
        throw new DashboardError("Invalid wireless cancellation response; outcome is uncertain", undefined, true);
      return result;
    }
    catch (error) {
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      throw error;
    }
  },
  changed: () => { if (currentSnapshot) renderStatus(currentSnapshot); },
});

function wirelessReady() {
  const snapshot = currentSnapshot;
  const operations = operationState(snapshot);
  return snapshot?.connected && Boolean(snapshot.hardware?.capabilities & 128)
    && !operations.busy && !wirelessForm.inFlight()
    && !operations.storageCommandPending && snapshot.storage_stale === false
    && snapshot.msc && !snapshot.msc.enabled && !snapshot.msc.transition_pending
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
    const result = await responseJson(response, `Wireless endpoint returned HTTP ${response.status}`, true);
    if (result.reboot_pending === true) publishCommandResult({ opcode: 5, status: "completed" });
    const saved = result.wireless;
    const expectedSsidHex = request.ssid_hex ??
      [...new TextEncoder().encode(request.ssid)].map(byte => byte.toString(16).padStart(2, "0")).join("");
    if (result.reboot_pending !== true || saved?.ssid_hex?.toLowerCase() !== expectedSsidHex.toLowerCase()
      || saved.wifi_enabled !== request.wifi_enabled || saved.ble_enabled !== request.ble_enabled
      || (request.tcp_cli_enabled !== undefined && saved.tcp_cli_enabled !== request.tcp_cli_enabled)
      || (request.ble_key !== undefined && saved.ble_key_configured !== Boolean(request.ble_key))
      || result.settings?.system_clock_mhz !== settings.system_clock_mhz
      || result.settings?.sdcard_profile !== settings.sdcard_profile || result.settings?.radios !== settings.radios) {
      throw new DashboardError("Wireless readback did not match the requested values", undefined, true);
    }
    savedSettings(result, settings);
    wirelessForm.finish(saved);
    return result.settings;
  } catch (error) {
    const uncertain = isUncertainOutcome(error) || hasUnknownOutcome(error)
      || (typeof error.uncertain !== "boolean" && error.status >= 500);
    if (uncertain && !error.uncertain) {
      error = new Error("Wireless provisioning outcome is uncertain; use Re-sync before retrying.");
      error.name = "UnknownOutcomeError";
      error.uncertain = true;
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
  return operationState().boardSelectionBusy;
}

function updateBoardSelectionControls() {
  boardSelect.disabled = boardSelectionBusy() || knownBoards.length === 0;
  rescanBoardsButton.disabled = boardSelectionBusy();
}

function updateCommandControls() {
  const operations = operationState();
  ledControl.disabled = !operations.canSetLed;
  mscControl.disabled = !operations.canChangeMsc;
}

function renderStatus(snapshot) {
  if (snapshot.connected) disconnectedUi = false;
  else if (!disconnectedUi) {
    disconnectedUi = true;
    shareDialog.close();
    wirelessForm.reset();
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = null;
  }
  const operations = operationState(snapshot);
  updateBoardSelectionControls();
  renderDashboardStatus({
    ...snapshot,
    error: snapshot.error ?? (uncertainCommand ? snapshot.command_resync_supported
      ? uncertainCommand.message : "Command outcome is uncertain. Reconnect the board and reload the dashboard before retrying."
      : commandFailure?.message ?? boardScanWarning),
    console_url: `/console?board=${encodeURIComponent(selectedBoard ?? "")}`,
    busy: operations.busy,
    no_board_selected: !selectedBoard,
    settings_busy: operations.settingsBusy,
    settings_dirty: settingsDirty,
    wireless_dirty: wirelessForm.dirty(),
    wireless_stage_valid: wirelessForm.stageValid(),
    wireless_save_block_reason: wirelessForm.saveBlockReason(),
    settings_storage_dirty: settingsStorageDirty,
    settings_message: settingsMessage ?? snapshot.settings_message,
    msc_command_pending: operations.mscPending,
    self_test_command_pending: operations.selfTestPending,
    msc_outcome_uncertain: mscError !== null && mscPendingEnabled !== null,
    status_read_error: statusReadError !== null,
    status_reading: requestInProgress,
    command_resyncing: commandResyncInProgress,
    command_recovery: operations.recovery,
    can_recover_command: operations.canRecover,
    outcome_uncertain: operations.outcomeUncertain,
    can_set_led: operations.canSetLed,
    can_change_msc: operations.canChangeMsc,
    can_start_self_test: operations.canStartSelfTest,
    can_cancel_self_test: operations.canCancelSelfTest,
    can_edit_self_test: operations.canEditSelfTest,
  });
  wirelessForm.update();
  if (wirelessReady() && snapshot.wireless) {
    wirelessForm.initialize(snapshot.wireless);
  }
}

async function resyncCommandOutcome() {
  const operations = operationState();
  if (!operations.canRecover) return;
  const version = selectionVersion;
  commandResyncInProgress = true;
  renderStatus(currentSnapshot);
  try {
    if (operations.recovery === "cleanup") {
      if (await wirelessForm.clearTest()) await updateStatus();
      return;
    }
    const response = await fetchWithDeadline(boardUrl("/api/control/resync"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ confirm: true }),
    }, 10000);
    const result = await responseJson(response, `Command re-sync returned HTTP ${response.status}`, true);
    if (version !== selectionVersion) return;
    const control = reconciledResult(result);
    uncertainCommand = null;
    ledError = null;
    currentSnapshot.command_outcome_uncertain = false;
    currentSnapshot.error = null;
    if (control) publishCommandResult(control);
    if (control?.opcode !== 10) wirelessForm.cancelResolved(false);
    if (result.wireless) currentSnapshot.wireless = result.wireless;
    if (result.control_result) currentSnapshot.control_result = result.control_result;
    saveBoardCommandState();
    await updateStatus();
  } catch (error) {
    if (version === selectionVersion && currentSnapshot) currentSnapshot.error = error.message;
  } finally {
    commandResyncInProgress = false;
    updateBoardSelectionControls();
    if (version === selectionVersion && currentSnapshot) renderStatus(currentSnapshot);
  }
}

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress || settingsRequestInProgress) return;
  requestInProgress = true;
  const version = selectionVersion;
  try {
    const response = await fetchWithDeadline(boardUrl("/api/status"), { cache: "no-store" }, 5000);
    const snapshot = normalizeStatus(await responseJson(response, `Status endpoint returned HTTP ${response.status}`));
    if (version !== selectionVersion) return;
    if (mscRequestInProgress || settingsRequestInProgress) return;
    statusReadError = null;
    if (rebootPending !== null) {
      if (!snapshot.connected) rebootPending = "disconnected";
      else if (snapshot.reboot_pending) rebootPending = "observed";
      else if (rebootPending !== "requested") rebootPending = null;
    }
    if (commandSettled(uncertainCommand, snapshot)) {
      if (uncertainCommand.kind === "settings") {
        settingsDirty = settingsStorageDirty = false;
        settingsMessage = "Settings readback matches the requested values.";
      }
      uncertainCommand = null;
      ledError = null;
    }
    currentSnapshot = snapshot;
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
    saveBoardCommandState();
    if (currentSnapshot.wireless_phase) wirelessProgress?.(currentSnapshot.wireless_phase);
    renderStatus(currentSnapshot);
  } catch (error) {
    if (version !== selectionVersion) return;
    // Both hosts return 404 for an unknown board; a restart requires discovery to register it again.
    if (error.status === 404
      && Date.now() - unknownBoardRescanAt >= 5000) {
      unknownBoardRescanAt = Date.now();
      void rescanBoards();
    }
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
    updateCommandControls();
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
  saveBoardCommandState();
  configurationDialog.close();
  rebootDialog.close();
  returnDialog.close();
  shareDialog.close();
  selectedBoard = nextBoard;
  const selected = knownBoards.find(board => board.id === selectedBoard);
  selectedBoardLabel = selected ? boardLabel(selected) : selectedBoardLabel;
  selectionVersion++;
  disconnectedUi = false;
  currentSnapshot = null;
  settingsDirty = false;
  settingsStorageDirty = false;
  settingsMessage = null;
  restoreBoardCommandState();
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
    boardScanWarning = Array.isArray(result.warnings) ? result.warnings.join(" ") || null : null;
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
    if (!selectedBoard && result.default_board_id) {
      saveBoardCommandState();
      selectedBoard = result.default_board_id;
    }
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
      disconnectedUi = false;
      currentSnapshot = null;
      settingsDirty = false;
      settingsStorageDirty = false;
      restoreBoardCommandState();
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
  if (!operationState().canChangeMsc) return;
  const force = currentSnapshot.msc.enabled;
  if ((confirmReturn && !force) || (confirmShare && force)) return;
  if (force) {
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
  clearCommandFailure(3);
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
    const result = await responseJson(response, `MSC endpoint returned HTTP ${response.status}`, true);
    if (version !== selectionVersion) return;
    const msc = result.msc ?? result;
    if (typeof msc.enabled !== "boolean")
      throw new DashboardError("MSC endpoint returned invalid ownership status", undefined, true);
    publishCommandResult({ opcode: 3, status: "completed" });
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
      if (mscPendingEnabled !== null) void updateStatus();
    } else void updateStatus();
  }
}

async function setLed() {
  if (!operationState().canSetLed) return;
  const version = selectionVersion;
  const requestedState = !currentSnapshot.runtime.led_on;
  ledRequestInProgress = true;
  clearCommandFailure(1);
  ledControl.disabled = true;
  try {
    const response = await fetchWithDeadline(boardUrl("/api/led"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ on: requestedState }),
    }, 10000);
    if (version !== selectionVersion) return;
    publishCommandResult(acknowledge(await responseJson(response, `LED endpoint returned HTTP ${response.status}`, true), 1));
    if (version === selectionVersion && currentSnapshot) {
      ledError = null;
      if (currentSnapshot.runtime) currentSnapshot.runtime = { ...currentSnapshot.runtime, led_on: requestedState };
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (version === selectionVersion && currentSnapshot) {
      const message = uncertainMessage(error, "LED command outcome is uncertain; refreshing board status.");
      if (markCommandOutcomeUncertain(error, message, { kind: "led", on: requestedState })) {
        ledError = currentSnapshot.error;
      } else currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    ledRequestInProgress = false;
    if (version === selectionVersion) {
      if (currentSnapshot) renderStatus(currentSnapshot);
      if (uncertainCommand) void updateStatus();
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
      || settingsRequestInProgress || storageCommandPending() || operationState().busy || !msc
      || msc.enabled !== force
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)) return;
  const combined = wirelessForm.dirty();
  if (combined && (force || !wirelessReady())) return;
  settingsRequestInProgress = true;
  clearCommandFailure(5);
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
    const result = await responseJson(response, `Settings endpoint returned HTTP ${response.status}`, true);
    publishCommandResult({ opcode: 5, status: "completed" });
    currentSnapshot.settings = savedSettings(result, settings);
    currentSnapshot.settings_supported = true;
    settingsDirty = false;
    settingsStorageDirty = false;
    settingsMessage = "Settings saved. Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    if (markCommandOutcomeUncertain(error, undefined,
      combined ? { kind: "resync" } : { kind: "settings", expected: settings })) {
      settingsMessage = combined
        ? "Wireless save outcome is uncertain; use Re-sync before retrying."
        : "Save outcome is uncertain; checking status before allowing another save.";
    } else settingsMessage = `Save failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    updateBoardSelectionControls();
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (uncertainCommand) void updateStatus();
  }
}

async function requestReboot() {
  const response = await fetchWithDeadline(boardUrl("/api/reboot"), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ confirm: true }),
  }, 15000);
  publishCommandResult(acknowledge(await responseJson(response, `Reboot endpoint returned HTTP ${response.status}`, true), 2));
}

async function rebootDevice() {
  if (!rebootDialog.open) return;
  rebootDialog.close();
  const msc = currentSnapshot?.msc;
  if (!currentSnapshot?.connected || operationState().busy || storageCommandPending()
      || !((currentSnapshot.hardware?.capabilities ?? 0) & 2)
      || !msc || msc.enabled) return;
  settingsRequestInProgress = true;
  clearCommandFailure(2);
  settingsMessage = "Requesting reboot...";
  renderStatus(currentSnapshot);
  try {
    await requestReboot();
    settingsMessage = "Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    if (markCommandOutcomeUncertain(error)) {
      settingsMessage = uncertainMessage(error,
        "Reboot outcome is uncertain; waiting for the board to reconnect before retrying.");
    } else settingsMessage = `Reboot failed: ${error.message}`;
  } finally {
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (uncertainCommand) void updateStatus();
  }
}

async function selfTest(action) {
  const operations = operationState();
  if (action === "start" ? !operations.canStartSelfTest : !operations.canCancelSelfTest) return;
  const version = selectionVersion;
  selfTestRequestInProgress = true;
  clearCommandFailure(4);
  selfTestPendingSequence = currentSnapshot.self_test?.sequence ?? 0;
  renderStatus(currentSnapshot);
  try {
    const response = await fetchWithDeadline(boardUrl("/api/self-test"), {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action, size_mib: document.querySelector("#self-test-size").valueAsNumber }),
    }, 10000);
    const control = acknowledge(await responseJson(response, `Self-test endpoint returned HTTP ${response.status}`, true), 4);
    if (version !== selectionVersion) return;
    publishCommandResult(control);
    selfTestPendingSequence = currentSnapshot?.self_test?.sequence ?? selfTestPendingSequence;
  } catch (error) {
    if (version !== selectionVersion) return;
    const sequence = selfTestPendingSequence;
    const uncertain = markCommandOutcomeUncertain(error, undefined, { kind: "self-test", sequence });
    if (!uncertain) selfTestPendingSequence = null;
    if (currentSnapshot) {
      currentSnapshot.error = uncertain
        ? uncertainMessage(error, "Benchmark command outcome is uncertain; checking for a new test sequence.")
        : error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    selfTestRequestInProgress = false;
    if (version === selectionVersion && currentSnapshot) renderStatus(currentSnapshot);
    if (uncertainCommand) void updateStatus();
  }
}
document.querySelector("#self-test-start").addEventListener("click", () => selfTest("start"));
document.querySelector("#self-test-cancel").addEventListener("click", () => selfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderStatus(currentSnapshot ?? {}));
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
commandResync.addEventListener("click", resyncCommandOutcome);
document.querySelector("#wifi-resync").addEventListener("click", resyncCommandOutcome);
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