function uptimeLabel(milliseconds) {
  if (!Number.isFinite(milliseconds)) return "--";
  const totalSeconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  return days ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${seconds}s`;
}

let lastStorageDetails = null;
let cardAbsent = false;

export function renderStatus(snapshot) {
  const connected = Boolean(snapshot.connected);
  if (!connected) {
    lastStorageDetails = null;
    cardAbsent = false;
  }
  const hardware = snapshot.hardware;
  const runtime = snapshot.telemetry_stale ? null : snapshot.runtime;
  const connection = document.querySelector("#connection");
  connection.dataset.state = snapshot.error ? "error" : connected ? "connected" : "disconnected";
  document.querySelector("#connection-label").textContent = snapshot.error
    ? "Attention" : snapshot.reconnecting ? "Reconnecting" : connected ? "Connected" : "Disconnected";
  document.querySelector("#product-name").textContent = hardware
    ? `${hardware.product_name} (${hardware.capabilities & 8 ? "SPI" : "SDIO"})` : "--";
  const boardModel = typeof hardware?.board_model === "string" ? hardware.board_model
    : ({ 1: "Pico", 2: "Pico 2", 3: "Pico 2 W", 4: "Pico W" })[hardware?.board_model]
      ?? (hardware ? `Unknown (${hardware.board_model})` : "--");
  document.querySelector("#board-model").textContent = hardware?.mcu
    ? `${boardModel} (${hardware.mcu})` : boardModel;
  document.querySelector("#firmware-version").textContent = hardware?.firmware_version ?? "Unknown";
  const clock = runtime
    ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--";
  document.querySelector("#identity-clock").textContent = clock;
  renderSettings(snapshot);
  document.querySelector("#cpu-usage-core0").textContent = runtime?.cpu_usage_core0_basis_points == null
    ? "--" : `${(runtime.cpu_usage_core0_basis_points / 100).toFixed(1)}%`;
  document.querySelector("#cpu-usage-core1").textContent = runtime?.cpu_usage_core1_basis_points == null
    ? "--" : `${(runtime.cpu_usage_core1_basis_points / 100).toFixed(1)}%`;
  const ramUsed = hardware?.ram_reserved_bytes;
  const ramTotal = hardware?.ram_total_bytes;
  document.querySelector("#ram-usage").textContent = Number.isSafeInteger(ramTotal) && ramTotal > 0
    && Number.isSafeInteger(ramUsed) && ramUsed >= 0 && ramUsed <= ramTotal
    ? `${(ramUsed / 1024).toFixed(1)} / ${(ramTotal / 1024).toFixed(0)} KiB`
      + ` (${(ramUsed * 100 / ramTotal).toFixed(1)}%)`
    : "--";
  document.querySelector("#serial").textContent = hardware?.serial_number || "--";
  document.querySelector("#uptime").textContent = runtime
    ? uptimeLabel(runtime.uptime_milliseconds ?? runtime.uptime_ms) : "--";
  setState("#cdc-state", runtime ? (runtime.cdc_open ? "Open" : "Closed") : "--", runtime?.cdc_open);
  const memory = snapshot.storage_stale ? null : snapshot.memory;
  if (memory?.presence_known === true) cardAbsent = memory.present === false;
  const storageShared = snapshot.msc_command_pending || snapshot.msc?.enabled || snapshot.msc?.transition_pending;
  document.querySelector('[aria-labelledby="memory-title"]').hidden = cardAbsent;
  document.querySelector('[aria-labelledby="self-test-title"]').hidden = cardAbsent || storageShared;
  const board = snapshot.board_status;
  document.querySelector("#temperature").textContent = board?.temperature_valid
    ? `${(board.temperature_centidegrees / 100).toFixed(2)} °C` : "--";
  const ledControl = document.querySelector("#led-control");
  const ledOn = board?.led_available ? Boolean(board.led_on) : null;
  ledControl.textContent = ledOn === null ? "Board LED: --" : `Board LED: ${ledOn ? "On" : "Off"}`;
  ledControl.dataset.state = ledOn === null ? "unknown" : ledOn ? "on" : "off";
  ledControl.setAttribute("aria-pressed", String(ledOn === true));
  ledControl.title = ledOn === null ? "Onboard LED unavailable"
    : `Turn onboard LED ${ledOn ? "off" : "on"}`;
  const msc = snapshot.msc;
  const mscControl = document.querySelector("#msc-control");
  mscControl.textContent = msc?.enabled ? "Return to Pico" : "Enable USB drive";
  mscControl.disabled = snapshot.settings_busy || snapshot.msc_command_pending || !connected || !msc || Boolean(msc.transition_pending)
    || Boolean(snapshot.self_test?.active) || Boolean(snapshot.self_test_command_pending)
    || Boolean(msc?.enabled && !msc.force_return_supported);
  document.querySelector("#msc-note").textContent = msc?.enabled && !msc.force_return_supported
    ? "Update firmware to disable the USB drive from Pico; this firmware requires manual host eject."
    : msc?.request_rejected
    ? (msc.enabled ? "Request refused. Disabling requires idle storage and successful device sync. Check diagnostics."
      : "Storage return failed. Volume may be unmounted; inspect media and UART errors before mounting.")
    : msc?.transition_pending ? "Storage handoff in progress; waiting for fresh ownership status."
      : "";
  document.querySelector("#msc-note").hidden = !document.querySelector("#msc-note").textContent;
  renderBoardConfiguration(snapshot);
  renderMemory(memory, snapshot.filesystem_format, Boolean(snapshot.storage_stale), snapshot.msc);
  renderSelfTest(snapshot);

  const message = snapshot.error ?? (snapshot.reconnecting ? "Reconnecting after USB-drive mode change..."
    : runtime ? "HID telemetry is live"
    : connected && snapshot.telemetry_stale ? "Telemetry report is stale"
    : connected ? "Waiting for first status report"
      : snapshot.no_board_selected ? "Select a PicoForge board" : "Connect a PicoForge board");
  const statusLine = document.querySelector(".status-line");
  statusLine.dataset.error = String(Boolean(snapshot.error));
  statusLine.dataset.active = String(Boolean(connected && runtime));
  document.querySelector("#status-message").textContent = message;
}

function renderSettings(snapshot) {
  const settings = snapshot.settings;
  const supported = Boolean(snapshot.connected && snapshot.settings_supported && settings);
  const msc = snapshot.msc;
  const storageBusy = snapshot.msc_command_pending || !msc || msc.transition_pending
    || snapshot.self_test?.active || snapshot.self_test_command_pending;
  const ready = supported && !storageBusy;
  const storageControl = document.querySelector("#settings-storage");
  updateClockOptions(document.querySelector("#settings-clock"), snapshot.hardware?.mcu,
    settings?.system_clock_mhz);
  if (settings && !snapshot.settings_dirty) {
    document.querySelector("#settings-clock").value = String(settings.system_clock_mhz);
  }
  if (settings && !snapshot.settings_storage_dirty) {
    storageControl.value = String(storageSpeed(settings));
  }
  const wireless = Boolean(snapshot.hardware?.capabilities & 128);
  for (const [id, radio] of [["settings-wifi", 1], ["settings-ble", 2]]) {
    const control = document.querySelector(`#${id}`);
    document.querySelector(`label[for="${id}"]`).hidden = !wireless;
    if (settings && !snapshot.settings_dirty) control.value = String(Number(Boolean((settings.radios ?? 0) & radio)));
  }
  for (const selector of ["#settings-clock", "#settings-storage", "#settings-wifi", "#settings-ble"]) {
    document.querySelector(selector).disabled = snapshot.settings_busy || !ready;
  }
  // Credentials have no forced-save path: provisioning requires firmware-owned storage.
  document.querySelector("#settings-save").disabled = snapshot.settings_busy || !ready
    || !(snapshot.settings_dirty || snapshot.wireless_dirty) || Boolean(snapshot.wireless_dirty && msc?.enabled)
    || !((snapshot.hardware?.capabilities ?? 0) & 2)
    || Boolean(msc?.enabled && !((snapshot.hardware?.capabilities ?? 0) & 64));
  document.querySelector("#reboot-control").disabled = snapshot.settings_busy || !snapshot.connected
    || !((snapshot.hardware?.capabilities ?? 0) & 2) || !snapshot.msc
    || snapshot.msc.enabled || snapshot.msc.transition_pending || snapshot.self_test?.active
    || snapshot.self_test_command_pending || snapshot.msc_command_pending;
  document.querySelector("#settings-note").textContent = snapshot.settings_message
    || (!snapshot.connected ? "Waiting for a PicoForge connection."
      : !snapshot.settings_supported ? "Boot settings unavailable; update device firmware."
        : !msc ? "Waiting for storage ownership status."
          : msc.enabled && !((snapshot.hardware?.capabilities ?? 0) & 64)
            ? "Update firmware or return storage to Pico before saving settings."
          : msc.transition_pending ? "Wait for the USB storage transition before saving settings."
            : "");
  document.querySelector("#settings-note").hidden = !document.querySelector("#settings-note").textContent;
  document.querySelector("#status-message").hidden = !snapshot.error
    && !document.querySelector("#settings-note").hidden;
}

function updateClockOptions(select, mcu, savedClock) {
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
  const options = [...choices, ...(unsupportedClock === null ? []
    : [[unsupportedClock, `${unsupportedClock} MHz (saved; unsupported)`, true]])]
    .map(([value, label, disabled = false]) => {
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

export function storageSpeed(settings) {
  return settings.sdcard_profile;
}

function renderBoardConfiguration(snapshot) {
  const configuration = snapshot.board_configuration;
  const set = (selector, value) => { document.querySelector(selector).textContent = value; };
  const formatPin = pin => pin === 255 || pin == null ? "Unassigned" : `GP${pin}`;
  const renderPins = (selector, signals, pins) => {
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
    document.querySelector(selector).replaceChildren(headings, values);
  };
  const spi = Boolean(snapshot.hardware?.capabilities & 8);
  const labels = spi ? ["CLK", "MOSI", "MISO", "CS"] : ["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3"];
  set("#board-uart-title", configuration && configuration.monitor_uart_index !== 255
    ? `Monitor UART${configuration.monitor_uart_index}` : "Monitor UART");
  set("#board-storage-title", configuration ? `Storage (${spi ? "SPI" : "SDIO-PIO"})` : "Storage");
  renderPins("#board-uart", ["TX", "RX"],
    [configuration?.monitor_uart_tx_gpio, configuration?.monitor_uart_rx_gpio]);
  renderPins("#board-storage-pins", [...labels, "Card detect"],
    [...labels.map((_, index) => configuration?.storage_gpios[index]), configuration?.card_detect_gpio]);
  if (!configuration) {
    set("#board-reset-cause", "--");
    return;
  }
  const resetCause = ({ 0: "Unknown", 1: "Power-on", 2: "Watchdog", 3: "Software request", 4: "Watchdog after HardFault" })[configuration.reset_cause]
    ?? `Unassigned (${configuration.reset_cause})`;
  set("#board-reset-cause", resetCause);
}

function renderMemory(memory, filesystemFormat, stale, msc) {
  const set = (id, value) => { document.querySelector(id).textContent = value; };
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
      ? "No card detected. Check card insertion and the active-high DET connection on GP8."
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
  const filesystemLabels = { unknown: "Unknown", fat12: "FAT12", fat16: "FAT16", fat32: "FAT32", exfat: "exFAT" };
  set("#memory-filesystem", filesystemLabels[filesystemFormat] ?? "Unknown");
  set("#memory-manufacturer", formatManufacturer(manufacturer));
  set("#memory-product", manufacturer
    ? `${manufacturer.product_name} rev ${manufacturer.product_revision} · ${manufacturer.manufacture_year}-${String(manufacturer.manufacture_month).padStart(2, "0")}`
    : "--");
    set("#memory-serial", manufacturer ? manufacturer.serial_number.toString(16).padStart(8, "0") : "--");
}

function formatManufacturer(manufacturer) {
  if (!manufacturer) return "--";
  const id = manufacturer.id ?? manufacturer.manufacturer_id;
  if (!Number.isInteger(id) || id < 0 || id > 0xff) return "Unknown manufacturer";
  const names = { 0x03: "SanDisk", 0x41: "Kingston" };
  return `${names[id] ?? "Unknown manufacturer"} (0x${id.toString(16).padStart(2, "0")})`;
}

function formatBytes(bytes) {
  const gibibytes = bytes / (1024 ** 3);
  return gibibytes >= 1 ? `${gibibytes.toFixed(2)} GiB`
    : bytes < 1048576 ? `${(bytes / 1024).toFixed(1)} KiB` : `${(bytes / (1024 ** 2)).toFixed(2)} MiB`;
}

function setState(selector, label, active) {
  const element = document.querySelector(selector);
  element.textContent = label;
  if (active === undefined) delete element.dataset.active;
  else element.dataset.active = String(active);
}

export function renderSelfTest(snapshot) {
  const status = snapshot.self_test;
  const set = (selector, value) => { document.querySelector(selector).textContent = value; };
  const speed = value => value == null ? "--" : `${(value / 1048576).toFixed(2)} MiB/s`;
  const count = value => `${(value / 1048576).toFixed(1)} MiB`;
  const labels = { idle: "Idle", queued: "Queued", mounting: "Mounting", writing: "Writing", reading: "Verifying", cleanup: "Cleanup", passed: "Passed", failed: "Failed", cancelled: "Cancelled" };
  const errors = ["", "Storage busy", "USB host owns the card", "Filesystem already mounted", "Invalid test size", "Card unavailable", "Insufficient free space", "PFTEST.BIN already exists", "Filesystem I/O failed", "Readback verification failed", "Cleanup failed", "Filesystem not mounted"];
  set("#self-test-state", status ? `${labels[status.state] ?? "Unknown"} · ${Math.floor(status.progress_percent ?? 0)}%`
    : "Unavailable");
  document.querySelector("#self-test-progress").value = status?.progress_percent ?? 0;
  document.querySelector("#self-test-meter").dataset.result = ["passed", "failed"].includes(status?.state)
    ? status.state : "";
  const disclosure = document.querySelector("#self-test-disclosure");
  const autoOpen = status?.active ? "active" : status?.state === "failed" ? "failed" : "";
  if (autoOpen && disclosure.dataset.autoOpen !== autoOpen) disclosure.open = true;
  disclosure.dataset.autoOpen = autoOpen;
  set("#self-test-write-speed", speed(status?.write_bytes_per_second));
  set("#self-test-read-speed", speed(status?.read_bytes_per_second));
  set("#self-test-written", status ? `${count(status.written_bytes)} / ${count(status.target_bytes)}` : "--");
  set("#self-test-read", status ? `${count(status.read_bytes)} / ${count(status.target_bytes)}` : "--");
  set("#self-test-write-time", status ? `${(status.write_elapsed_ms / 1000).toFixed(2)} s` : "--");
  set("#self-test-read-time", status ? `${(status.read_elapsed_ms / 1000).toFixed(2)} s` : "--");
  const notices = [];
  if (status?.request_error) notices.push(`Request rejected: ${errors[status.request_error]}.`);
  if (status?.error) notices.push(`${errors[status.error]} (FatFs ${status.filesystem_result}).`);
  if (status?.file_remains) notices.push("PFTEST.BIN remains on the card.");
  if (status?.mismatch_count) notices.push(`${status.mismatch_count} mismatched bytes.`);
  set("#self-test-message", notices.join(" "));
  const pending = Boolean(snapshot.self_test_command_pending);
  const mounted = ["fat12", "fat16", "fat32", "exfat"].includes(snapshot.filesystem_format);
  const ready = snapshot.connected && snapshot.memory?.initialized && mounted && status && !snapshot.msc?.enabled;
  const size = document.querySelector("#self-test-size");
  const validSize = Number.isInteger(size.valueAsNumber) && size.valueAsNumber >= 1 && size.valueAsNumber <= 1024;
  document.querySelector("#self-test-start").disabled = !ready || status.active || pending || Boolean(snapshot.busy)
    || !validSize || Boolean(snapshot.msc?.transition_pending) || Boolean(snapshot.msc_command_pending);
  document.querySelector("#self-test-cancel").disabled = !snapshot.connected || !status?.active || status.state === "cleanup"
    || pending || Boolean(snapshot.busy) || Boolean(snapshot.msc_command_pending);
  size.disabled = !snapshot.connected || Boolean(status?.active) || pending || Boolean(snapshot.msc_command_pending);
}