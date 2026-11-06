// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
import { initializeSettings, normalizeStatus, renderStatus as renderDashboardStatus } from "./dashboard.mjs";
import { bindWirelessForm } from "./wireless-form.mjs";
import {
  acknowledge, DashboardError, isObject, reconciledResult, responseJson, savedSettings, savedWireless,
  scannedBoards, scannedNetworks, statusResponse, testedWireless,
  wirelessConfiguration,
} from "./dashboard-api.mjs";
import {
  commandEffects, createCommandState, observeCommandStatus, operationState as deriveOperationState, unresolvedCommand,
} from "./operation-state.mjs";

const ledControl = /** @type {HTMLButtonElement} */ (document.querySelector("#led-control"));
const mscControl = /** @type {HTMLButtonElement} */ (document.querySelector("#msc-control"));
const mscStatusRefresh = /** @type {HTMLButtonElement} */ (document.querySelector("#msc-status-refresh"));
const commandResync = /** @type {HTMLButtonElement} */ (document.querySelector("#command-resync"));
const boardSelect = /** @type {HTMLSelectElement} */ (document.querySelector("#board-select"));
const rescanBoardsButton = /** @type {HTMLButtonElement} */ (document.querySelector("#rescan-boards"));
const settingsSave = /** @type {HTMLButtonElement} */ (document.querySelector("#settings-save"));
const configurationDialog = /** @type {HTMLDialogElement} */ (document.querySelector("#configuration-dialog"));
const rebootDialog = /** @type {HTMLDialogElement} */ (document.querySelector("#reboot-dialog"));
const returnDialog = /** @type {HTMLDialogElement} */ (document.querySelector("#return-dialog"));
const shareDialog = /** @type {HTMLDialogElement} */ (document.querySelector("#share-dialog"));
let configurationForceSave = false;
/** @type {string | null} */
let configurationIdentity = null;
const clockControl = /** @type {HTMLSelectElement} */ (document.querySelector("#settings-clock"));
const storageControl = /** @type {HTMLSelectElement} */ (document.querySelector("#settings-storage"));
const selfTestSize = /** @type {HTMLInputElement} */ (document.querySelector("#self-test-size"));
const rebootControl = /** @type {HTMLButtonElement} */ (document.querySelector("#reboot-control"));
/** @param {string} id */
function element(id) {
  const node = /** @type {HTMLElement | null} */ (document.querySelector(`#${id}`));
  if (!node) throw new Error(`Missing dashboard element: ${id}`);
  return node;
}
/** @param {string} url @param {RequestInit} options @param {number} timeoutMs */
const fetchWithDeadline = (url, options, timeoutMs) =>
  fetch(url, { ...options, signal: AbortSignal.timeout(timeoutMs) });
let requestInProgress = false;
/** @typedef {import("./dashboard-api.mjs").StatusSnapshot} DashboardSnapshot
 * @typedef {{id: string, serial_number?: string | null, device_path: string}} Board
 */
/** @type {DashboardSnapshot | null} */
let currentSnapshot = null;
let disconnectedUi = false;
let selfTestRequestInProgress = false;
let ledRequestInProgress = false;
let commandResyncInProgress = false;
let mscRequestInProgress = false;
let settingsRequestInProgress = false;
/** @type {Partial<Pick<import("./dashboard-api.mjs").BootSettings, "system_clock_mhz" | "sdcard_profile">>} */
let settingsDraft = {};
/** @type {string | null} */
let settingsMessage = null;
/** @type {string | null} */
let selectedBoard = null;
let selectedBoardLabel = "";
/** @type {Board[]} */
let knownBoards = [];
let scanningBoards = false;
/** @type {string | null} */
let boardScanWarning = null;
let unknownBoardRescanAt = 0;
// Selection fences board ownership; command versions reject observations started before a local transition.
let selectionVersion = 0;
let commandVersion = 0;
/** @type {((phase: string) => void) | null} */
let wirelessProgress = null;
/** @type {string | null} */
let statusReadError = null;
let boardState = createCommandState();
/** @type {Map<string, import("./operation-state.mjs").BoardCommandState>} */
const boardCommandStates = new Map();
/** @param {string} boardId */
function commandStateFor(boardId) {
  let state = boardCommandStates.get(boardId);
  if (!state) { state = createCommandState(); boardCommandStates.set(boardId, state); }
  return state;
}
/** @param {unknown} error */
function hasUnknownOutcome(error) {
  return error instanceof Error && ["AbortError", "TimeoutError", "UnknownOutcomeError"].includes(error.name)
    || error instanceof TypeError;
}
/** @param {unknown} error */
function isUncertainOutcome(error) {
  if (error instanceof DashboardError && typeof error.uncertain === "boolean") return error.uncertain;
  return error instanceof Error && /uncertain|outcome.*not resent/i.test(error.message);
}
/** @param {Error} error @param {string} fallback */
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
    if (boardId) commandStateFor(boardId).uncertainCommand = command;
    return true;
  }
  boardState.uncertainCommand = command;
  if (currentSnapshot) {
    currentSnapshot.error = message;
  }
  return true;
}
/** @typedef {import("./operation-state.mjs").OperationSnapshot} OperationSnapshot */
/** @typedef {import("./operation-state.mjs").OperationState} OperationState */
/** @param {OperationSnapshot | null} [snapshot] @returns {OperationState} */
function operationState(snapshot = currentSnapshot) {
  return deriveOperationState(snapshot, {
    uncertain: boardState.uncertainCommand !== null, rebootPending: boardState.rebootPending !== null,
    resyncing: commandResyncInProgress, saving: settingsRequestInProgress,
    wirelessBusy: wirelessForm.operationBusy(), wirelessInFlight: wirelessForm.inFlight(),
    statusFailed: statusReadError !== null, mscInFlight: mscRequestInProgress,
    mscPending: boardState.mscPendingEnabled !== null,
    selfTestInFlight: selfTestRequestInProgress, selfTestPending: boardState.selfTestPendingSequence !== null,
    ledInFlight: ledRequestInProgress, scanning: scanningBoards,
    radiosDirty: wirelessForm.radioEdited("wifi") || wirelessForm.radioEdited("ble"),
    settingsDirty: settingsDirty(), wirelessDirty: wirelessForm.dirty(),
    wirelessBlockReason: wirelessForm.saveBlockReason(),
  }, selfTestSize.valueAsNumber);
}

/** Apply the same confirmed lifecycle effects for ordinary acknowledgements and explicit Re-sync.
 * @param {import("./dashboard-api.mjs").ControlResult} result
 */
function publishCommandResult(result) {
  const effects = commandEffects(result);
  if (effects.releaseMsc) {
    boardState.mscPendingEnabled = null;
    boardState.mscError = null;
  }
  if (effects.releaseSelfTest) boardState.selfTestPendingSequence = null;
  if (effects.completed && result.opcode === 4)
    boardState.selfTestPendingSequence = "awaiting-status";
  if (effects.reboot) boardState.rebootPending = "requested";
  if (effects.wifiCancel) {
    if (effects.completed && currentSnapshot) currentSnapshot.wireless_cleanup_required = false;
    wirelessForm.cancelResolved(effects.completed);
  }
  if (effects.failure) boardState.commandFailure = { opcode: result.opcode, message: effects.failure };
  else clearCommandFailure(result.opcode);
}
/** @param {number} opcode */
function clearCommandFailure(opcode) {
  if (boardState.commandFailure?.opcode === opcode) boardState.commandFailure = null;
}
function storageCommandPending(snapshot = currentSnapshot) {
  return operationState(snapshot).storageCommandPending;
}
const wirelessForm = bindWirelessForm({
  supported: () => Boolean((currentSnapshot?.hardware?.capabilities ?? 0) & 128),
  cleanupRequired: () => Boolean(currentSnapshot?.wireless_cleanup_required),
  ready: wirelessReady,
  scan: async () => {
    const boardId = selectedBoard;
    commandVersion++;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/scan", boardId), { method: "POST" }, 20000);
      const result = await responseJson(response, `Wireless scan returned HTTP ${response.status}`, true);
      return scannedNetworks(result);
    } catch (error) {
      if (!(error instanceof Error)) throw error;
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      throw error;
    } finally {
      commandVersion++;
    }
  },
  test: async (settings, onProgress) => {
    const version = selectionVersion;
    const boardId = selectedBoard;
    commandVersion++;
    wirelessProgress = onProgress;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/test"), {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
      }, 40000);
      const result = await responseJson(response, `Connection test returned HTTP ${response.status}`, true);
      return testedWireless(result.test);
    } catch (error) {
      if (!(error instanceof Error)) throw error;
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      if (error instanceof TypeError || ["TimeoutError", "AbortError"].includes(error?.name)) {
        throw new Error(error.name === "TimeoutError"
          ? "Wi-Fi test timed out; result is uncertain. Use Re-sync before retrying."
          : "Wi-Fi test result was lost. Use Re-sync before retrying.");
      }
      throw error;
    } finally {
      commandVersion++;
      if (version === selectionVersion) wirelessProgress = null;
    }
  },
  cancelTest: async () => {
    const boardId = selectedBoard;
    commandVersion++;
    try {
      const response = await fetchWithDeadline(boardUrl("/api/wireless/cancel"), { method: "POST" }, 10000);
      const result = await responseJson(response, `Cancel returned HTTP ${response.status}`, true);
      if (result.cancelled !== true)
        throw new DashboardError("Invalid wireless cancellation response; outcome is uncertain", undefined, true);
      return result;
    }
    catch (error) {
      if (!(error instanceof Error)) throw error;
      markCommandOutcomeUncertain(error, undefined, { kind: "resync" }, boardId);
      throw error;
    } finally {
      commandVersion++;
    }
  },
  changed: () => { if (currentSnapshot) renderStatus(currentSnapshot); },
});

function wirelessReady() {
  return operationState().canProvisionWireless && !wirelessForm.inFlight();
}

/** Save board settings and credentials in one firmware commit; failures before delivery stay retryable.
 * @param {import("./dashboard-api.mjs").BootSettings} settings
 */
async function saveCombinedSettings(settings) {
  const request = { ...wirelessForm.request(), wifi_enabled: Boolean(settings.radios & 1),
    ble_enabled: Boolean(settings.radios & 2), board: settings };
  try {
    const response = await fetchWithDeadline(boardUrl("/api/wireless"), {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request),
    }, 15000);
    const result = await responseJson(response, `Wireless endpoint returned HTTP ${response.status}`, true);
    if (result.reboot_pending === true) publishCommandResult({ opcode: 5, status: "completed" });
    const saved = savedWireless(result, request);
    const confirmed = savedSettings(result, settings);
    wirelessForm.finish(saved);
    return confirmed;
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    const uncertain = isUncertainOutcome(error) || hasUnknownOutcome(error)
      || (error instanceof DashboardError && typeof error.uncertain !== "boolean" && (error.status ?? 0) >= 500);
    const failure = uncertain && !(error instanceof DashboardError && error.uncertain)
      ? new DashboardError("Wireless provisioning outcome is uncertain; use Re-sync before retrying.", undefined, true)
      : error;
    wirelessForm.finish(null, failure.message);
    throw failure;
  }
}

/** @param {string} path @param {string | null} [boardId] */
function boardUrl(path, boardId = selectedBoard) {
  if (!boardId) return path;
  return `${path}${path.includes("?") ? "&" : "?"}board=${encodeURIComponent(boardId)}`;
}

/** @param {Board} board */
function boardLabel(board) {
  return board.serial_number ? `PicoForge / ${board.serial_number}` : `PicoForge / HID ${board.device_path}`;
}

function boardSelectionBusy() {
  return operationState().boardSelectionBusy;
}

/** @param {OperationState} [operations] */
function updateBoardSelectionControls(operations = operationState()) {
  boardSelect.disabled = operations.boardSelectionBusy || knownBoards.length === 0;
  rescanBoardsButton.disabled = operations.boardSelectionBusy;
}

function updateCommandControls() {
  const operations = operationState();
  ledControl.disabled = !operations.canSetLed;
  mscControl.disabled = !operations.canChangeMsc;
}

/** @param {DashboardSnapshot} snapshot */
function acceptConnection(snapshot) {
  if (snapshot.connected === true) disconnectedUi = false;
  else if (snapshot.connected === false && !disconnectedUi) {
    disconnectedUi = true;
    shareDialog.close();
    wirelessForm.reset();
    settingsDraft = {};
    settingsMessage = null;
  }
}

function settingsEditState() {
  return {
    settings_dirty: settingsDirty(),
    settings_draft: selectedSettings(),
  };
}

/** @param {DashboardSnapshot} snapshot */
function renderStatus(snapshot) {
  const operations = operationState(snapshot);
  updateBoardSelectionControls(operations);
  renderDashboardStatus({
    ...snapshot,
    error: snapshot.error ?? (boardState.uncertainCommand ? snapshot.command_resync_supported
      ? boardState.uncertainCommand.message
        : "Command outcome is uncertain. Reconnect the board and reload the dashboard before retrying."
      : boardState.commandFailure?.message ?? boardScanWarning),
    console_url: `/console?board=${encodeURIComponent(selectedBoard ?? "")}`,
    no_board_selected: !selectedBoard,
    ...settingsEditState(),
    wireless_dirty: wirelessForm.dirty(),
    settings_message: settingsMessage ?? snapshot.settings_message,
    msc_outcome_uncertain: boardState.mscError !== null && boardState.mscPendingEnabled !== null,
    status_read_error: statusReadError !== null,
    status_reading: requestInProgress,
  }, operations);
  const discard = /** @type {HTMLButtonElement} */ (element("wireless-discard"));
  discard.hidden = !wirelessForm.conflicted();
  discard.disabled = !operations.canProvisionWireless;
  wirelessForm.update();
}

element("wireless-discard").addEventListener("click", async () => {
  if (!operationState().canProvisionWireless || !wirelessForm.conflicted()) return;
  commandVersion++;
  try {
    if (!await wirelessForm.discardEdits() || !currentSnapshot) return;
    currentSnapshot.wireless = null;
    renderStatus(currentSnapshot);
  } finally {
    commandVersion++;
    await updateStatus();
  }
});

async function resyncCommandOutcome() {
  const operations = operationState();
  if (!operations.canRecover || !currentSnapshot) return;
  const version = selectionVersion;
  commandVersion++;
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
    boardState.uncertainCommand = unresolvedCommand(control, boardState.mscPendingEnabled !== null,
      boardState.selfTestPendingSequence !== null, boardState.uncertainCommand);
    if (boardState.uncertainCommand?.kind === "reconnect-status") boardState.mscError = null;
    boardState.ledError = null;
    currentSnapshot.command_outcome_uncertain = false;
    currentSnapshot.error = null;
    if (control) publishCommandResult(control);
    if (control?.opcode !== 10) wirelessForm.cancelResolved(false);
    if (result.wireless != null) currentSnapshot.wireless = wirelessConfiguration(result.wireless);
    if (result.control_result) currentSnapshot.control_result = result.control_result;
    await updateStatus();
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (version === selectionVersion && currentSnapshot) currentSnapshot.error = error.message;
  } finally {
    commandVersion++;
    commandResyncInProgress = false;
    updateBoardSelectionControls();
    if (version === selectionVersion && currentSnapshot) renderStatus(currentSnapshot);
  }
}

async function updateStatus() {
  if (requestInProgress || mscRequestInProgress || settingsRequestInProgress) return;
  requestInProgress = true;
  const version = selectionVersion;
  const command = commandVersion;
  try {
    const response = await fetchWithDeadline(boardUrl("/api/status"), { cache: "no-store" }, 5000);
    /** @type {DashboardSnapshot} */
    const snapshot = normalizeStatus(statusResponse(
      await responseJson(response, `Status endpoint returned HTTP ${response.status}`)));
    if (version !== selectionVersion || command !== commandVersion) return;
    if (mscRequestInProgress || settingsRequestInProgress) return;
    statusReadError = null;
    if (!commandResyncInProgress
      && observeCommandStatus(boardState, snapshot, selfTestRequestInProgress)?.kind === "settings") {
      settingsDraft = {};
      settingsMessage = "Settings readback matches the requested values.";
    }
    currentSnapshot = snapshot;
    acceptConnection(snapshot);
    currentSnapshot.error = boardState.mscError ?? boardState.ledError ?? currentSnapshot.error;
    if (currentSnapshot.wireless_phase) wirelessProgress?.(currentSnapshot.wireless_phase);
    if (wirelessReady() && currentSnapshot.wireless) wirelessForm.initialize(currentSnapshot.wireless);
    initializeSettings({ ...snapshot, ...settingsEditState() });
    renderStatus(currentSnapshot);
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (version !== selectionVersion || command !== commandVersion) return;
    // Both hosts return 404 for an unknown board; a restart requires discovery to register it again.
    if (error instanceof DashboardError && error.status === 404
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
    if (version !== selectionVersion || command !== commandVersion) void updateStatus();
  }
}

/** @param {string} boardId */
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
  disconnectedUi = false;
  currentSnapshot = null;
  acceptConnection({ connected: false });
  settingsDraft = {};
  settingsMessage = null;
  boardState = selectedBoard ? commandStateFor(selectedBoard) : createCommandState();
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
    knownBoards = scannedBoards(result);
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
    if (!selectedBoard && typeof result.default_board_id === "string") {
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
      acceptConnection({ connected: false });
      settingsDraft = {};
      boardState = selectedBoard ? commandStateFor(selectedBoard) : createCommandState();
      renderStatus({ connected: false });
    }
    await updateStatus();
  } catch (error) {
    if (!(error instanceof Error)) throw error;
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
  if (!operationState().canChangeMsc || !currentSnapshot?.msc) return;
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
  commandVersion++;
  mscRequestInProgress = true;
  clearCommandFailure(3);
  boardState.mscPendingEnabled = !force;
  boardState.mscError = null;
  renderStatus(currentSnapshot);
  mscControl.disabled = true;
  element("msc-note").textContent = force ? "Revoking host access and attempting remount..."
    : "Preparing storage for USB...";
  element("msc-note").hidden = false;
  try {
    const response = await fetchWithDeadline(boardUrl(force ? "/api/msc/force-return" : "/api/msc"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(force ? { confirm_data_loss: true } : { enabled: !force }),
    }, 15000);
    if (version !== selectionVersion) return;
    const result = await responseJson(response, `MSC endpoint returned HTTP ${response.status}`, true);
    if (version !== selectionVersion) return;
    const msc = result.msc ?? result;
    if (!isObject(msc) || typeof msc.enabled !== "boolean")
      throw new DashboardError("MSC endpoint returned invalid ownership status", undefined, true);
    publishCommandResult({ opcode: 3, status: "completed" });
    if (version === selectionVersion && currentSnapshot) {
      currentSnapshot.msc = { enabled: msc.enabled,
        transition_pending: msc.transition_pending === true, request_rejected: msc.request_rejected === true };
      currentSnapshot.error = null;
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (version === selectionVersion) {
      const uncertain = markCommandOutcomeUncertain(error,
        "Storage command outcome is uncertain. Use Re-sync or wait for fresh ownership status before retrying.",
        { kind: "msc", enabled: !force });
      if (!uncertain) boardState.mscPendingEnabled = null;
      boardState.mscError = uncertain
        ? "Storage command outcome is uncertain; waiting for updated ownership status before retrying."
        : error.message;
      if (currentSnapshot) {
        currentSnapshot.error = boardState.mscError;
        renderStatus(currentSnapshot);
      }
    }
  } finally {
    commandVersion++;
    mscRequestInProgress = false;
    if (version === selectionVersion) {
      if (currentSnapshot) renderStatus(currentSnapshot);
      if (boardState.mscPendingEnabled !== null) void updateStatus();
    } else void updateStatus();
  }
}

async function setLed() {
  if (!operationState().canSetLed || !currentSnapshot?.runtime) return;
  const version = selectionVersion;
  const requestedState = !currentSnapshot.runtime.led_on;
  commandVersion++;
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
      boardState.ledError = null;
      if (currentSnapshot.runtime) currentSnapshot.runtime = { ...currentSnapshot.runtime, led_on: requestedState };
      renderStatus(currentSnapshot);
    }
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (version === selectionVersion && currentSnapshot) {
      const message = uncertainMessage(error, "LED command outcome is uncertain; refreshing board status.");
      if (markCommandOutcomeUncertain(error, message, { kind: "led", on: requestedState })) {
        boardState.ledError = currentSnapshot.error ?? null;
      } else currentSnapshot.error = error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    commandVersion++;
    ledRequestInProgress = false;
    if (version === selectionVersion) {
      if (currentSnapshot) renderStatus(currentSnapshot);
      if (boardState.uncertainCommand) void updateStatus();
    } else void updateStatus();
  }
}

function selectedSettings() {
  const saved = currentSnapshot?.settings;
  return {
    system_clock_mhz: settingsDraft.system_clock_mhz ?? saved?.system_clock_mhz ?? 0,
    sdcard_profile: settingsDraft.sdcard_profile ?? saved?.sdcard_profile ?? 0,
    radios: selectedRadios(),
  };
}

function selectedRadios() {
  const saved = currentSnapshot?.settings?.radios ?? 0;
  return (currentSnapshot?.hardware?.capabilities ?? 0) & 128
    ? wirelessForm.selectedRadios(saved) : 0;
}

function settingsDirty() {
  const saved = currentSnapshot?.settings;
  const selected = selectedSettings();
  return Boolean(saved && (selected.system_clock_mhz !== saved.system_clock_mhz
    || selected.sdcard_profile !== saved.sdcard_profile || selected.radios !== saved.radios));
}

function saveIdentity() {
  return JSON.stringify([selectedSettings(), wirelessForm.saveIdentity()]);
}

async function saveSettings() {
  if (!configurationDialog.open) return;
  const confirmedIdentity = configurationIdentity;
  configurationDialog.close();
  const force = configurationForceSave;
  const operations = operationState();
  if (!currentSnapshot || !operations.canSaveSettings || currentSnapshot.msc?.enabled !== force) {
    settingsMessage = operations.saveBlockReason ?? "Storage ownership changed. Review the settings and confirm again.";
    if (currentSnapshot) renderStatus(currentSnapshot);
    return;
  }
  if (confirmedIdentity !== saveIdentity()) {
    settingsMessage = "The proposed configuration changed. Review the settings and confirm again.";
    renderStatus(currentSnapshot);
    return;
  }
  const combined = wirelessForm.dirty();
  commandVersion++;
  settingsRequestInProgress = true;
  clearCommandFailure(5);
  settingsMessage = "Saving boot settings...";
  const settings = selectedSettings();
  renderStatus(currentSnapshot);
  try {
    if (combined) {
      currentSnapshot.settings = await saveCombinedSettings(settings);
      settingsDraft = {};
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
    settingsDraft = {};
    settingsMessage = "Settings saved. Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (markCommandOutcomeUncertain(error, undefined,
      combined ? { kind: "resync" } : { kind: "settings", expected: settings })) {
      settingsMessage = combined
        ? "Wireless save outcome is uncertain; use Re-sync before retrying."
        : "Save outcome is uncertain; checking status before allowing another save.";
    } else settingsMessage = `Save failed: ${error.message}`;
  } finally {
    commandVersion++;
    settingsRequestInProgress = false;
    updateBoardSelectionControls();
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (boardState.uncertainCommand) void updateStatus();
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
  commandVersion++;
  settingsRequestInProgress = true;
  clearCommandFailure(2);
  settingsMessage = "Requesting reboot...";
  renderStatus(currentSnapshot);
  try {
    await requestReboot();
    settingsMessage = "Reboot requested. Waiting for the device to reconnect.";
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (markCommandOutcomeUncertain(error)) {
      settingsMessage = uncertainMessage(error,
        "Reboot outcome is uncertain; waiting for the board to reconnect before retrying.");
    } else settingsMessage = `Reboot failed: ${error.message}`;
  } finally {
    commandVersion++;
    settingsRequestInProgress = false;
    if (currentSnapshot) renderStatus(currentSnapshot);
    if (boardState.uncertainCommand) void updateStatus();
  }
}

/** @param {"start" | "cancel"} action */
async function selfTest(action) {
  const operations = operationState();
  if (!currentSnapshot || (action === "start" ? !operations.canStartSelfTest : !operations.canCancelSelfTest)) return;
  const version = selectionVersion;
  commandVersion++;
  selfTestRequestInProgress = true;
  clearCommandFailure(4);
  boardState.selfTestPendingSequence = currentSnapshot.self_test?.sequence ?? 0;
  renderStatus(currentSnapshot);
  try {
    const response = await fetchWithDeadline(boardUrl("/api/self-test"), {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ action, size_mib: selfTestSize.valueAsNumber }),
    }, 10000);
    const control = acknowledge(await responseJson(response, `Self-test endpoint returned HTTP ${response.status}`, true), 4);
    if (version !== selectionVersion) return;
    publishCommandResult(control);
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    if (version !== selectionVersion) return;
    const uncertain = markCommandOutcomeUncertain(error, undefined, { kind: "self-test" });
    if (!uncertain) boardState.selfTestPendingSequence = null;
    if (currentSnapshot) {
      currentSnapshot.error = uncertain
        ? uncertainMessage(error, "Benchmark command outcome is uncertain. Use Re-sync before retrying.")
        : error.message;
      renderStatus(currentSnapshot);
    }
  } finally {
    commandVersion++;
    selfTestRequestInProgress = false;
    if (version === selectionVersion && currentSnapshot) renderStatus(currentSnapshot);
    if (boardState.uncertainCommand) void updateStatus();
  }
}
element("self-test-start").addEventListener("click", () => selfTest("start"));
element("self-test-cancel").addEventListener("click", () => selfTest("cancel"));
element("self-test-size").addEventListener("input", () => renderStatus(currentSnapshot ?? {}));
boardSelect.addEventListener("change", () => selectBoard(boardSelect.value));
rescanBoardsButton.addEventListener("click", rescanBoards);
/** @type {Array<[HTMLSelectElement, "system_clock_mhz" | "sdcard_profile"]>} */
const settingsControls = [[clockControl, "system_clock_mhz"], [storageControl, "sdcard_profile"]];
for (const [control, field] of settingsControls) control.addEventListener("change", () => {
  const value = Number(control.value);
  if (!Number.isInteger(value) || value < 0) throw new Error(`Invalid ${field}: ${control.value}`);
  if (value === currentSnapshot?.settings?.[field]) delete settingsDraft[field];
  else settingsDraft[field] = value;
  settingsMessage = null;
  if (currentSnapshot) renderStatus(currentSnapshot);
});
settingsSave.addEventListener("click", () => {
  if (!operationState().canSaveSettings) return;
  configurationForceSave = Boolean(currentSnapshot?.msc?.enabled);
  element("configuration-force-warning").hidden = !configurationForceSave;
  element("configuration-wireless-warning").hidden = !wirelessForm.dirty();
  const settings = selectedSettings();
  const credentials = wirelessForm.credentialActions();
  configurationIdentity = saveIdentity();
  element("configuration-dialog-profile").textContent =
    `System clock: ${settings.system_clock_mhz ? `${settings.system_clock_mhz} MHz` : "Default"}. `
    + `SD Card: ${settings.sdcard_profile === 1 ? "High-Speed" : "Standard"}.`
    + ((currentSnapshot?.hardware?.capabilities ?? 0) & 128 ? ` Wi-Fi: ${settings.radios & 1 ? "Enabled" : "Disabled"}.`
      + ` Bluetooth: ${settings.radios & 2 ? "Enabled" : "Disabled"}.`
      + ` Wi-Fi credentials: ${credentials.wifi}. Bluetooth key: ${credentials.ble}.` : "");
  configurationDialog.showModal();
});
rebootControl.addEventListener("click", () => {
  if (rebootControl.disabled) return;
  element("reboot-consequence").textContent = settingsDirty() || wirelessForm.dirty()
    ? "Unsaved dashboard changes will be lost." : "Saved next-boot settings will be applied.";
  rebootDialog.showModal();
});
element("cancel-configuration").addEventListener("click", () => configurationDialog.close());
element("cancel-reboot").addEventListener("click", () => rebootDialog.close());
element("cancel-return").addEventListener("click", () => returnDialog.close());
element("cancel-share").addEventListener("click", () => shareDialog.close());
element("confirm-share").addEventListener("click", () => setMsc(false, true));
element("confirm-return").addEventListener("click", () => setMsc(true));
mscStatusRefresh.addEventListener("click", updateStatus);
commandResync.addEventListener("click", resyncCommandOutcome);
element("wifi-resync").addEventListener("click", resyncCommandOutcome);
configurationDialog.addEventListener("close", () => { configurationIdentity = null; });
for (const dialog of [configurationDialog, rebootDialog, returnDialog, shareDialog]) {
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      event.preventDefault();
      dialog.close();
    }
  });
}
element("confirm-configuration").addEventListener("click", saveSettings);
element("confirm-reboot").addEventListener("click", rebootDevice);
ledControl.addEventListener("click", setLed);
mscControl.addEventListener("click", () => setMsc());
void rescanBoards();
const pollWhenVisible = () => {
  return document.hidden ? Promise.resolve() : updateStatus();
};
document.addEventListener?.("visibilitychange", pollWhenVisible);
window.setInterval(pollWhenVisible, 1000);