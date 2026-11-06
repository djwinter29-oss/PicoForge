// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
import { renderWirelessStatus } from "./wireless-form.mjs";
import { operationState } from "./operation-state.mjs";

/**
 * @typedef {import("./dashboard-api.mjs").StatusSnapshot & {
 * console_url?: string, settings_draft?: import("./dashboard-api.mjs").BootSettings,
 * settings_dirty?: boolean, wireless_dirty?: boolean, no_board_selected?: boolean,
 * msc_outcome_uncertain?: boolean, status_read_error?: boolean, status_reading?: boolean}} DashboardView
 */
/** @param {string} selector */
function element(selector) {
  const node = /** @type {HTMLElement | null} */ (document.querySelector(selector));
  if (!node) throw new Error(`Missing dashboard element: ${selector}`);
  return node;
}
/** @param {string} selector */
const select = selector => /** @type {HTMLSelectElement} */ (element(selector));
/** @param {string} selector */
const button = selector => /** @type {HTMLButtonElement} */ (element(selector));
/** @param {number | undefined} milliseconds */
function uptimeLabel(milliseconds) {
  if (milliseconds === undefined || !Number.isFinite(milliseconds)) return "--";
  const totalSeconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  return days ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${seconds}s`;
}

/** @type {{memory: import("./dashboard-api.mjs").Memory, filesystemFormat: string | null | undefined} | null} */
let lastStorageDetails = null;
let cardAbsent = false;
/** @type {string | null} */
let boardConfigurationSignature = null;
/** @type {{uartTitle: HTMLElement, storageTitle: HTMLElement, uartTable: HTMLElement,
 * storageTable: HTMLElement, resetCause: HTMLElement} | null} */
let boardConfigurationElements = null;
/** @type {Document | null} */
let boardConfigurationDocument = null;

/**
 * Lift the view's shortcuts out of the shared status shape (tests/status-contract.json): both servers nest USB-drive
 * ownership and filesystem format in `memory` and next-boot settings in `board_configuration`.
 * @param {import("./dashboard-api.mjs").StatusSnapshot} snapshot
 * @returns {import("./dashboard-api.mjs").StatusSnapshot}
 */
export function normalizeStatus(snapshot) {
  const configuration = snapshot.settings_supported ? snapshot.board_configuration : null;
  return {
    ...snapshot,
    msc: snapshot.memory?.msc ?? null,
    filesystem_format: snapshot.memory?.filesystem_format ?? null,
    settings: (configuration ? {
      system_clock_mhz: configuration.system_clock_mhz, sdcard_profile: configuration.sdcard_profile,
      radios: configuration.radios, tcp_cli_enabled: configuration.tcp_cli_enabled,
    } : null),
  };
}

/** @param {DashboardView} snapshot @param {import("./operation-state.mjs").OperationState} [operations] */
export function renderStatus(snapshot, operations = operationState(snapshot, {
  settingsDirty: snapshot.settings_dirty, wirelessDirty: snapshot.wireless_dirty,
}, (/** @type {HTMLInputElement} */ (element("#self-test-size"))).valueAsNumber)) {
  const connected = Boolean(snapshot.connected);
  if (!connected) {
    lastStorageDetails = null;
    cardAbsent = false;
  }
  const hardware = snapshot.hardware;
  const runtime = snapshot.telemetry_stale ? null : snapshot.runtime;
  const connection = element("#connection");
  connection.dataset.state = snapshot.error ? "error" : connected ? "connected" : "disconnected";
  element("#connection-label").textContent = snapshot.error
    ? "Attention" : snapshot.reconnecting ? "Reconnecting" : connected ? "Connected" : "Disconnected";
  element("#product-name").textContent = hardware?.product_name !== undefined
    ? `${hardware.product_name} (${(hardware.capabilities ?? 0) & 8 ? "SPI" : "SDIO"})` : "--";
  const boardModel = typeof hardware?.board_model === "string" ? hardware.board_model
    : ({ 1: "Pico", 2: "Pico 2", 3: "Pico 2 W", 4: "Pico W" })[hardware?.board_model ?? 0]
      ?? (hardware?.board_model !== undefined ? `Unknown (${hardware.board_model})` : "--");
  element("#board-model").textContent = hardware?.mcu
    ? `${boardModel} (${hardware.mcu})` : boardModel;
  element("#firmware-version").textContent = hardware?.firmware_version ?? "Unknown";
  const clock = runtime?.system_clock_hz !== undefined
    ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--";
  element("#identity-clock").textContent = clock;
  renderSettings(snapshot, operations);
  element("#cpu-usage-core0").textContent = runtime?.cpu_usage_core0_basis_points == null
    ? "--" : `${(runtime.cpu_usage_core0_basis_points / 100).toFixed(1)}%`;
  element("#cpu-usage-core1").textContent = runtime?.cpu_usage_core1_basis_points == null
    ? "--" : `${(runtime.cpu_usage_core1_basis_points / 100).toFixed(1)}%`;
  const ramUsed = hardware?.ram_reserved_bytes;
  const ramTotal = hardware?.ram_total_bytes;
  element("#ram-usage").textContent = typeof ramTotal === "number" && Number.isSafeInteger(ramTotal) && ramTotal > 0
    && typeof ramUsed === "number" && Number.isSafeInteger(ramUsed) && ramUsed >= 0 && ramUsed <= ramTotal
    ? `${(ramUsed / 1024).toFixed(1)} / ${(ramTotal / 1024).toFixed(0)} KiB`
      + ` (${(ramUsed * 100 / ramTotal).toFixed(1)}%)`
    : "--";
  element("#serial").textContent = hardware?.serial_number || "--";
  element("#uptime").textContent = runtime
    ? uptimeLabel(runtime.uptime_milliseconds ?? runtime.uptime_ms) : "--";
  setState("#cdc-state", runtime?.cdc_open === undefined ? "--" : runtime.cdc_open ? "Open" : "Closed",
    runtime?.cdc_open);
  renderWirelessStatus(snapshot);
  const memory = snapshot.storage_stale ? null : snapshot.memory;
  if (memory?.presence_known === true) cardAbsent = memory.present === false;
  const storageShared = Boolean(operations.mscPending || snapshot.msc?.enabled || snapshot.msc?.transition_pending);
  element('[aria-labelledby="memory-title"]').hidden = cardAbsent;
  element('[aria-labelledby="self-test-title"]').hidden = cardAbsent || storageShared;
  element("#temperature").textContent = runtime?.temperature_valid && runtime.temperature_centidegrees !== undefined
    ? `${(runtime.temperature_centidegrees / 100).toFixed(2)} °C` : "--";
  const ledControl = button("#led-control");
  const ledOn = runtime?.led_available ? Boolean(runtime.led_on) : null;
  ledControl.textContent = ledOn === null ? "Board LED: --" : `Board LED: ${ledOn ? "On" : "Off"}`;
  ledControl.dataset.state = ledOn === null ? "unknown" : ledOn ? "on" : "off";
  ledControl.setAttribute("aria-pressed", String(ledOn === true));
  ledControl.disabled = !operations.canSetLed;
  ledControl.title = ledOn === null ? "Onboard LED unavailable"
    : `Turn onboard LED ${ledOn ? "off" : "on"}`;
  const msc = snapshot.msc;
  const mscControl = button("#msc-control");
  mscControl.textContent = msc?.enabled ? "Return to Pico" : "Enable USB drive";
  mscControl.disabled = !operations.canChangeMsc;
  element("#msc-note").textContent = snapshot.msc_outcome_uncertain
    ? "Storage command outcome is uncertain. Check fresh ownership status before taking action."
    : msc?.request_rejected
    ? (msc.enabled ? "Request refused. Disabling requires idle storage and successful device sync. Check diagnostics."
      : "Storage return failed. Volume may be unmounted; inspect media and UART errors before mounting.")
    : msc?.transition_pending ? "Storage handoff in progress; waiting for fresh ownership status."
      : operations.storageBlockReason ?? "";
  element("#msc-note").hidden = !element("#msc-note").textContent;
  const mscStatusRefresh = button("#msc-status-refresh");
  mscStatusRefresh.hidden = !snapshot.msc_outcome_uncertain;
  mscStatusRefresh.disabled = Boolean(snapshot.status_read_error || snapshot.status_reading);
  renderBoardConfiguration(snapshot);
  renderMemory(memory, snapshot.filesystem_format, Boolean(snapshot.storage_stale), snapshot.msc,
    snapshot.board_configuration?.card_detect_gpio);
  renderSelfTest(snapshot, operations);

  const message = snapshot.error ?? (snapshot.wireless_cleanup_required
    ? "Temporary Wi-Fi credentials need cleanup. Clear the Wi-Fi test before sending another command."
    : snapshot.reconnecting ? "Reconnecting after USB-drive mode change..."
    : runtime ? "HID telemetry is live"
    : connected && snapshot.telemetry_stale ? "Telemetry report is stale"
    : connected ? "Waiting for first status report"
      : snapshot.no_board_selected ? "Select a PicoForge board" : "Connect a PicoForge board");
  const statusLine = element(".status-line");
  statusLine.dataset.error = String(Boolean(snapshot.error || snapshot.diagnostic_error));
  statusLine.dataset.active = String(Boolean(connected && runtime));
  element("#status-message").textContent = [message, snapshot.diagnostic_error].filter(Boolean).join(" ");
  const commandResync = button("#command-resync");
  if (commandResync) {
    const recovery = operations.recovery;
    commandResync.textContent = recovery === "cleanup" ? "Clear Wi-Fi test" : "Re-sync";
    commandResync.hidden = recovery === null;
    commandResync.disabled = !operations.canRecover;
    const wifiResync = button("#wifi-resync");
    if (wifiResync) {
      wifiResync.hidden = recovery !== "resync";
      wifiResync.disabled = commandResync.disabled;
    }
  }
}

/** Project accepted settings plus owned field drafts into the controls. */
/** @param {DashboardView} snapshot */
export function initializeSettings(snapshot) {
  const settings = snapshot.settings_draft ?? snapshot.settings;
  const storageControl = select("#settings-storage");
  updateClockOptions(select("#settings-clock"), snapshot.hardware?.mcu,
    settings?.system_clock_mhz);
  if (settings) {
    select("#settings-clock").value = String(settings.system_clock_mhz);
    storageControl.value = String(storageSpeed(settings));
  }
}

/** @param {DashboardView} snapshot @param {import("./operation-state.mjs").OperationState} operations */
function renderSettings(snapshot, operations) {
  const settings = snapshot.settings;
  const supported = Boolean(snapshot.connected && snapshot.settings_supported && settings);
  const msc = snapshot.msc;
  const storageBusy = operations.storageCommandPending;
  const ready = supported && !storageBusy;
  const wireless = Boolean((snapshot.hardware?.capabilities ?? 0) & 128);
  for (const id of ["wifi-settings", "ble-settings"])
    element(`#${id}`).hidden = !wireless;
  for (const selector of ["#settings-clock", "#settings-storage", "#settings-wifi", "#settings-ble"]) {
    (/** @type {HTMLSelectElement} */ (element(selector))).disabled = operations.settingsBusy || !ready
      || (selector === "#settings-wifi" || selector === "#settings-ble") && !operations.wirelessStatusReady;
  }
  const canSave = operations.canSaveSettings;
  const saveBlockReason = operations.saveBlockReason;
  button("#settings-save").disabled = !canSave;
  button("#settings-save").title = saveBlockReason ?? "";
  button("#reboot-control").disabled = operations.settingsBusy || !snapshot.connected
    || !((snapshot.hardware?.capabilities ?? 0) & 2) || !msc || msc.enabled || storageBusy;
  element("#settings-note").textContent = snapshot.settings_message
    || ((snapshot.settings_dirty || snapshot.wireless_dirty) ? saveBlockReason : null)
    || (!snapshot.connected ? "Waiting for a PicoForge connection."
      : !snapshot.settings_supported ? "Boot settings unavailable; update device firmware."
        : !msc ? "Waiting for storage ownership status."
          : msc.transition_pending ? "Wait for the USB storage transition before saving settings."
            : operations.storageBlockReason ?? operations.wirelessBlockReason ?? "");
  element("#settings-note").hidden = !element("#settings-note").textContent;
  element("#status-message").hidden = !snapshot.error && !snapshot.diagnostic_error
    && !element("#settings-note").hidden;
}

/** @param {HTMLSelectElement} select @param {string | undefined} mcu @param {number | undefined} savedClock */
function updateClockOptions(select, mcu, savedClock) {
  /** @type {Array<[number, string, boolean?]>} */
  const choices = mcu === "RP2040"
    ? [[0, "Default (125 MHz)"], [150, "150 MHz"], [200, "200 MHz"], [250, "250 MHz"]]
    : mcu === "RP2350"
      ? [[0, "Default (150 MHz)"], [200, "200 MHz"], [250, "250 MHz"]]
      : [[0, "Default"]];
  const unsupportedClock = Number.isInteger(savedClock) && savedClock !== 0
    && !choices.some(([value]) => value === savedClock) ? savedClock : null;
  const signature = `${mcu ?? "unknown"}:${unsupportedClock ?? ""}`;
  if (select.dataset.clockOptions === signature
    || typeof document.createElement !== "function" || typeof select.replaceChildren !== "function") return;

  const previous = select.value;
  /** @type {Array<[number, string, boolean?]>} */
  const allChoices = [...choices];
  if (unsupportedClock !== null && unsupportedClock !== undefined)
    allChoices.push([unsupportedClock, `${unsupportedClock} MHz (saved; unsupported)`, true]);
  const options = allChoices.map(([value, label, disabled = false]) => {
      const option = document.createElement("option");
      option.value = String(value);
      option.textContent = label;
      option.disabled = disabled;
      return option;
    });
  select.replaceChildren(...options);
  select.dataset.clockOptions = signature;
  select.value = options.some(option => option.value === previous) ? previous : "0";
}

/** @param {import("./dashboard-api.mjs").BootSettings} settings */
export function storageSpeed(settings) {
  return settings.sdcard_profile;
}

/** @param {DashboardView} snapshot */
function renderBoardConfiguration(snapshot) {
  if (boardConfigurationDocument !== document || !boardConfigurationElements) {
    boardConfigurationDocument = document;
    boardConfigurationSignature = null;
    boardConfigurationElements = {
      uartTitle: element("#board-uart-title"),
      storageTitle: element("#board-storage-title"),
      uartTable: element("#board-uart"),
      storageTable: element("#board-storage-pins"),
      resetCause: element("#board-reset-cause"),
    };
  }
  const { uartTitle, storageTitle, uartTable, storageTable, resetCause } = boardConfigurationElements;
  const configuration = snapshot.board_configuration;
  /** @param {HTMLElement} element @param {string} value */
  const setText = (element, value) => {
    if (element.textContent !== value) element.textContent = value;
  };
  /** @param {number | undefined} pin */
  const formatPin = pin => pin === undefined ? "--" : pin === 255 ? "Unassigned" : `GP${pin}`;
  /** @param {HTMLElement} table @param {string[]} signals @param {(number | undefined)[]} pins */
  const renderPins = (table, signals, pins) => {
    const headings = document.createElement("tr");
    const values = document.createElement("tr");
    signals.forEach((signal, index) => {
      const heading = document.createElement("th");
      heading.scope = "col";
      heading.textContent = signal;
      const cell = document.createElement("td");
      cell.textContent = configuration ? formatPin(pins[index]) : "--";
      headings.append(heading);
      values.append(cell);
    });
    table.replaceChildren(headings, values);
  };
  const spi = Boolean((snapshot.hardware?.capabilities ?? 0) & 8);
  const labels = spi ? ["CLK", "MOSI", "MISO", "CS"] : ["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3"];
  const signature = JSON.stringify([spi, configuration]);
  if (signature !== boardConfigurationSignature) {
    boardConfigurationSignature = signature;
    setText(uartTitle, configuration?.monitor_uart_index !== undefined && configuration.monitor_uart_index !== 255
      ? `Monitor UART${configuration.monitor_uart_index}` : "Monitor UART");
    setText(storageTitle, configuration ? `Storage (${spi ? "SPI" : "SDIO-PIO"})` : "Storage");
    renderPins(uartTable, ["TX", "RX"],
      [configuration?.monitor_uart_tx_gpio, configuration?.monitor_uart_rx_gpio]);
    renderPins(storageTable, [...labels, "Card detect"],
      [...labels.map((_, index) => configuration?.storage_gpios?.[index]), configuration?.card_detect_gpio]);
  }
  if (configuration?.reset_cause === undefined) {
    setText(resetCause, "--");
    return;
  }
  /** @type {Record<number, string>} */
  const resetCauses = { 0: "Unknown", 1: "Power-on", 2: "Watchdog", 3: "Software request", 4: "Watchdog after HardFault" };
  const resetCauseLabel = resetCauses[configuration.reset_cause]
    ?? `Unassigned (${configuration.reset_cause})`;
  setText(resetCause, resetCauseLabel);
}

/** @param {import("./dashboard-api.mjs").Memory | null | undefined} memory
 * @param {string | null | undefined} filesystemFormat @param {boolean} stale
 * @param {import("./dashboard-api.mjs").Msc | null | undefined} msc @param {number | undefined} cardDetectGpio
 */
function renderMemory(memory, filesystemFormat, stale, msc, cardDetectGpio) {
  /** @param {string} id @param {string} value */
  const set = (id, value) => { element(id).textContent = value; };
  const busy = !stale && memory?.configured && memory.sample_valid === false
    && !msc?.enabled && !msc?.transition_pending;
  if (!stale && memory?.sample_valid && memory.initialized && !msc?.enabled && !msc?.transition_pending) {
    lastStorageDetails = { memory, filesystemFormat };
  } else if (!busy) {
    lastStorageDetails = null;
  }
  if (stale) {
    set("#memory-note", "Storage status is stale; waiting for a fresh report.");
    for (const id of ["#memory-state", "#memory-speed", "#memory-size",
      "#memory-filesystem", "#memory-manufacturer", "#memory-product", "#memory-serial"]) set(id, "--");
    return;
  }
  set("#memory-state", msc?.enabled ? "USB host"
    : msc?.transition_pending ? "Transitioning"
      : memory?.configured && memory.sample_valid === false ? "Busy"
        : memory?.initialized ? "Ready" : memory?.configured ? "Not initialized" : "Unavailable");
  set("#memory-note", !memory?.configured ? "No SD-card transport configured"
    : msc?.enabled ? "The host owns the SD card through USB mass storage."
      : msc?.transition_pending ? "Storage status is unavailable during the ownership transition."
        : memory && !memory.sample_valid ? ""
    : memory.presence_known && !memory.present
      ? `No card detected. Check card insertion and the active-high DET connection${
        cardDetectGpio !== undefined && Number.isInteger(cardDetectGpio) && cardDetectGpio >= 0 && cardDetectGpio < 255
          ? ` on GP${cardDetectGpio}` : ""}.`
      : memory.present && !memory.initialized
        ? "Card detected but not initialized; check UART logs and SD wiring."
        : memory.initialized ? "" : "Card presence has not been established.");
  if (busy && lastStorageDetails) {
    ({ memory, filesystemFormat } = lastStorageDetails);
  }
  const manufacturer = memory?.manufacturer;
  set("#memory-speed", memory?.sample_valid !== false && memory?.initialized
    && !msc?.enabled && !msc?.transition_pending && memory?.bus_clock_hz
    ? `${(memory.bus_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--");
  set("#memory-size", memory?.capacity_bytes ? formatBytes(memory.capacity_bytes) : "--");
  /** @type {Record<string, string>} */
  const filesystemLabels = { unknown: "Unknown", fat12: "FAT12", fat16: "FAT16", fat32: "FAT32", exfat: "exFAT" };
  set("#memory-filesystem", filesystemLabels[filesystemFormat ?? "unknown"] ?? "Unknown");
  set("#memory-manufacturer", formatManufacturer(manufacturer));
  set("#memory-product", manufacturer?.product_name !== undefined && manufacturer.product_revision !== undefined
    && manufacturer.manufacture_year !== undefined && manufacturer.manufacture_month !== undefined
    ? `${manufacturer.product_name} rev ${manufacturer.product_revision} · ${manufacturer.manufacture_year}-${String(manufacturer.manufacture_month).padStart(2, "0")}`
    : "--");
  set("#memory-serial", manufacturer?.serial_number !== undefined
    ? manufacturer.serial_number.toString(16).padStart(8, "0") : "--");
}

/** @param {import("./dashboard-api.mjs").Manufacturer | null | undefined} manufacturer */
function formatManufacturer(manufacturer) {
  if (!manufacturer) return "--";
  const id = manufacturer.id ?? manufacturer.manufacturer_id;
  if (id === undefined || !Number.isInteger(id) || id < 0 || id > 0xff) return "Unknown manufacturer";
  /** @type {Record<number, string>} */
  const names = { 0x03: "SanDisk", 0x41: "Kingston" };
  return `${names[id] ?? "Unknown manufacturer"} (0x${id.toString(16).padStart(2, "0")})`;
}

/** @param {number} bytes */
function formatBytes(bytes) {
  const gibibytes = bytes / (1024 ** 3);
  return gibibytes >= 1 ? `${gibibytes.toFixed(2)} GiB`
    : bytes < 1048576 ? `${(bytes / 1024).toFixed(1)} KiB` : `${(bytes / (1024 ** 2)).toFixed(2)} MiB`;
}

/** @param {string} selector @param {string} label @param {boolean | undefined} active */
function setState(selector, label, active) {
  const node = element(selector);
  node.textContent = label;
  if (active === undefined) delete node.dataset.active;
  else node.dataset.active = String(active);
}

/** @param {DashboardView} snapshot @param {import("./operation-state.mjs").OperationState} [operations] */
export function renderSelfTest(snapshot, operations = operationState(snapshot, {},
  (/** @type {HTMLInputElement} */ (element("#self-test-size"))).valueAsNumber)) {
  const status = snapshot.self_test;
  /** @param {string} selector @param {string} value */
  const set = (selector, value) => { element(selector).textContent = value; };
  /** @param {number | undefined} value */
  const speed = value => value == null ? "--" : `${(value / 1048576).toFixed(2)} MiB/s`;
  /** @param {number | undefined} value */
  const count = value => value === undefined ? "--" : `${(value / 1048576).toFixed(1)} MiB`;
  /** @type {Record<string, string>} */
  const labels = { idle: "Idle", queued: "Queued", preparing: "Preparing", writing: "Writing", reading: "Verifying",
    cleanup: "Cleanup", passed: "Passed", failed: "Failed", cancelled: "Cancelled" };
  const errors = ["", "Storage busy", "USB host owns the card", "Invalid test size", "Card unavailable", "Insufficient free space", "PFTEST.BIN already exists", "Filesystem I/O failed", "Readback verification failed", "Cleanup failed", "Filesystem not mounted"];
  set("#self-test-state", status ? `${labels[status.state ?? ""] ?? "Unknown"} · ${Math.floor(status.progress_percent ?? 0)}%`
    : "Unavailable");
  (/** @type {HTMLProgressElement} */ (element("#self-test-progress"))).value = status?.progress_percent ?? 0;
  element("#self-test-meter").dataset.result = status?.state === "passed" || status?.state === "failed"
    ? status.state : "";
  const disclosure = /** @type {HTMLDetailsElement} */ (element("#self-test-disclosure"));
  const autoOpen = status?.active ? "active" : status?.state === "failed" ? "failed" : "";
  if (autoOpen && disclosure.dataset.autoOpen !== autoOpen) disclosure.open = true;
  disclosure.dataset.autoOpen = autoOpen;
  set("#self-test-write-speed", speed(status?.write_bytes_per_second));
  set("#self-test-read-speed", speed(status?.read_bytes_per_second));
  set("#self-test-written", status ? `${count(status.written_bytes)} / ${count(status.target_bytes)}` : "--");
  set("#self-test-read", status ? `${count(status.read_bytes)} / ${count(status.target_bytes)}` : "--");
  set("#self-test-write-time", status?.write_elapsed_ms !== undefined
    ? `${(status.write_elapsed_ms / 1000).toFixed(2)} s` : "--");
  set("#self-test-read-time", status?.read_elapsed_ms !== undefined
    ? `${(status.read_elapsed_ms / 1000).toFixed(2)} s` : "--");
  const notices = [];
  if (status?.request_error) notices.push(`Request rejected: ${errors[status.request_error]}.`);
  if (status?.error) notices.push(`${errors[status.error]} (FatFs ${status.filesystem_result}).`);
  if (status?.file_remains) notices.push("PFTEST.BIN remains on the card.");
  if (status?.mismatch_count) notices.push(`${status.mismatch_count} mismatched bytes.`);
  set("#self-test-message", notices.join(" "));
  const size = (/** @type {HTMLInputElement} */ (element("#self-test-size")));
  button("#self-test-start").disabled = !operations.canStartSelfTest;
  button("#self-test-cancel").disabled = !operations.canCancelSelfTest;
  size.disabled = !operations.canEditSelfTest;
}