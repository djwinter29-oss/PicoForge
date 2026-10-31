function uptimeLabel(milliseconds) {
  if (typeof milliseconds === "bigint") {
    const seconds = milliseconds / 1000n;
    const days = seconds / 86400n;
    const hours = seconds % 86400n / 3600n;
    const minutes = seconds % 3600n / 60n;
    return days > 0n ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${seconds % 60n}s`;
  }
  if (!Number.isFinite(milliseconds)) return "--";
  const seconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = seconds % 60;
  return days > 0 ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${remainder}s`;
}

function renderLinkState(selector, label, active) {
  const element = document.querySelector(selector);
  element.textContent = label;
  if (active === undefined) delete element.dataset.active;
  else element.dataset.active = String(active);
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
  const runtime = snapshot.runtime;
  updateClockOptions(document.querySelector("#settings-clock"), hardware?.mcu,
    snapshot.settings?.system_clock_mhz);
  const connection = document.querySelector("#connection");
  connection.dataset.state = snapshot.error ? "error" : connected ? "connected" : "disconnected";
  document.querySelector("#connection-label").textContent = snapshot.error
    ? "Attention" : snapshot.reconnecting ? "Reconnecting" : connected ? "Connected" : "Disconnected";
  document.querySelector("#product-name").textContent = hardware
    ? `${hardware.product_name} (${hardware.capabilities & 8 ? "SPI" : "SDIO"})` : "--";
  document.querySelector("#board-model").textContent = hardware
    ? `${hardware.board_name} (${hardware.mcu})` : "--";
  document.querySelector("#firmware-version").textContent = hardware?.firmware_version ?? "Unknown";
  const clock = runtime
    ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--";
  document.querySelector("#identity-clock").textContent = clock;
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
  document.querySelector("#uptime").textContent = runtime ? uptimeLabel(runtime.uptime_ms) : "--";
  renderLinkState("#cdc-state", runtime ? (runtime.cdc_open ? "Open" : "Closed") : "--", runtime?.cdc_open);
  const memory = snapshot.memory;
  if (memory?.presence_known === true) cardAbsent = memory.present === false;
  const storageShared = snapshot.msc?.enabled || snapshot.msc?.transition_pending || snapshot.msc_command_pending;
  document.querySelector('[aria-labelledby="memory-title"]').hidden = cardAbsent;
  document.querySelector('[aria-labelledby="self-test-title"]').hidden = cardAbsent || storageShared
    || !(hardware?.capabilities & 32);
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
  renderMemory(snapshot.memory, snapshot.filesystem_format, snapshot.msc);
  renderSelfTest(snapshot);
  const statusLine = document.querySelector(".status-line");
  statusLine.dataset.error = String(Boolean(snapshot.error));
  statusLine.dataset.active = String(Boolean(connected && runtime));
  document.querySelector("#status-message").textContent = snapshot.error
    ?? (snapshot.reboot_pending ? "Reboot accepted; waiting for USB reconnection"
      : snapshot.reconnecting ? "USB device reconnecting; reopening HID automatically"
      : runtime ? "HID status is live" : connected ? "Waiting for first status report" : "Select a PicoForge device");
  for (const [id, label] of [[3, "telemetry"], [10, "storage"], [11, "self-test"]]) {
    const status = document.querySelector(`#${label}-freshness`);
    status.textContent = id === 11 && !(hardware?.capabilities & 32)
      ? "Self-test unavailable" : `${label}: ${snapshot.freshness?.[id] ?? "unavailable"}`;
    status.hidden = snapshot.freshness?.[id] === "fresh";
  }
  renderBoardConfiguration(snapshot);
}

function renderBoardConfiguration(snapshot) {
  const config = snapshot.settings;
  const spi = Boolean(snapshot.hardware?.capabilities & 8);
  const signals = spi ? ["CLK", "MOSI", "MISO", "CS"] : ["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3"];
  const renderPins = (selector, labels, pins) => {
    const headings = document.createElement("tr");
    const values = document.createElement("tr");
    labels.forEach((label, index) => {
      const heading = document.createElement("th");
      heading.scope = "col";
      heading.textContent = label;
      const cell = document.createElement("td");
      cell.textContent = !config ? "--" : pins[index] === 255 || pins[index] == null
        ? "Unassigned" : `GP${pins[index]}`;
      headings.append(heading);
      values.append(cell);
    });
    document.querySelector(selector).replaceChildren(headings, values);
  };
  document.querySelector("#board-uart-title").textContent = config && config.monitor_uart_index !== 255
    ? `Monitor UART${config.monitor_uart_index}` : "Monitor UART";
  document.querySelector("#board-storage-title").textContent = config
    ? `Storage (${spi ? "SPI" : "SDIO-PIO"})` : "Storage";
  renderPins("#board-uart", ["TX", "RX"], [config?.monitor_uart_tx_gpio, config?.monitor_uart_rx_gpio]);
  renderPins("#board-storage-pins", [...signals, "Card detect"],
    [...signals.map((_, index) => config?.storage_gpios?.[index]), config?.card_detect_gpio]);
  document.querySelector("#board-reset-cause").textContent = config
    ? ["Unknown", "Power-on", "Watchdog", "Software request", "Watchdog after HardFault"][config.reset_cause]
      ?? `Unassigned (${config.reset_cause})` : "--";
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

function renderMemory(memory, filesystemFormat, msc) {
  const set = (id, value) => { document.querySelector(id).textContent = value; };
  const busy = memory?.configured && memory.sample_valid === false
    && !msc?.enabled && !msc?.transition_pending;
  if (memory?.sample_valid && memory.initialized && !msc?.enabled && !msc?.transition_pending) {
    lastStorageDetails = { memory, filesystemFormat };
  }
  else if (!busy) lastStorageDetails = null;
  set("#memory-state", msc?.enabled ? "USB host" : msc?.transition_pending ? "Transitioning" : busy ? "Busy"
    : memory?.initialized ? "Ready" : memory?.configured ? "Not initialized" : "Unavailable");
  set("#memory-note", !memory ? "Waiting for fresh storage status."
    : !memory.sample_valid ? ""
      : !memory.configured ? "No SD-card transport configured"
    : memory.presence_known && !memory.present
      ? "No card detected. Check card insertion and the configured card-detect connection."
      : memory.present && !memory.initialized
        ? "Card detected but not initialized; check UART logs and SD wiring."
        : memory.initialized ? "" : "Card presence has not been established.");
  document.querySelector("#memory-note").hidden = !document.querySelector("#memory-note").textContent;
  if (busy && lastStorageDetails) ({ memory, filesystemFormat } = lastStorageDetails);
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
  const id = manufacturer.id;
  if (!Number.isInteger(id) || id < 0 || id > 0xff) return "Unknown manufacturer";
  const names = { 0x03: "SanDisk", 0x41: "Kingston" };
  return `${names[id] ?? "Unknown manufacturer"} (0x${id.toString(16).padStart(2, "0")})`;
}

function formatBytes(bytes) {
  const gibibytes = bytes / (1024 ** 3);
  return gibibytes >= 1 ? `${gibibytes.toFixed(2)} GiB`
    : bytes < 1048576 ? `${(bytes / 1024).toFixed(1)} KiB` : `${(bytes / (1024 ** 2)).toFixed(2)} MiB`;
}

export function renderSelfTest(snapshot) {
  const status = snapshot.self_test;
  const set = (selector, value) => { document.querySelector(selector).textContent = value; };
  const speed = value => value == null ? "--" : `${(value / 1048576).toFixed(2)} MiB/s`;
  const count = value => `${(value / 1048576).toFixed(1)} MiB`;
  const labels = { idle: "Idle", queued: "Queued", preparing: "Preparing", writing: "Writing", reading: "Verifying", cleanup: "Cleanup", passed: "Passed", failed: "Failed", cancelled: "Cancelled" };
  const errors = ["", "Storage busy", "USB host owns the card", "Filesystem already mounted", "Invalid test size", "Card unavailable", "Insufficient free space", "PFTEST.BIN already exists", "Filesystem I/O failed", "Readback verification failed", "Cleanup failed", "Filesystem not mounted"];
  set("#self-test-state", status ? `${labels[status.state] ?? status.state} · ${Math.floor(status.progress_percent ?? 0)}%`
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
  if (status?.request_error) notices.push(`Request rejected: ${errors[status.request_error]
    ?? `Unknown error (${status.request_error})`}.`);
  if (status?.error) notices.push(`${errors[status.error] ?? `Unknown error (${status.error})`} (FatFs ${status.filesystem_result}).`);
  if (status && !status.valid_for_control) notices.push("Unknown status values; controls disabled.");
  if (status?.file_remains) notices.push("PFTEST.BIN remains on the card.");
  if (status?.mismatch_count) notices.push(`${status.mismatch_count} mismatched bytes.`);
  set("#self-test-message", notices.join(" "));
  const pending = Boolean(snapshot.self_test_command_pending);
  const fresh = snapshot.freshness?.[10] === "fresh" && snapshot.freshness?.[11] === "fresh";
  const ready = snapshot.connected && fresh && Boolean(snapshot.hardware?.capabilities & 32)
    && snapshot.memory?.initialized && status?.volume_mounted && status.valid_for_control
    && !snapshot.msc?.enabled && !snapshot.outcome_uncertain && !snapshot.reboot_pending;
  const size = document.querySelector("#self-test-size");
  const validSize = Number.isInteger(size.valueAsNumber) && size.valueAsNumber >= 1 && size.valueAsNumber <= 1024;
  document.querySelector("#self-test-start").disabled = !ready || status.active || pending || Boolean(snapshot.busy)
    || !validSize || Boolean(snapshot.msc?.transition_pending) || Boolean(snapshot.msc_command_pending);
  document.querySelector("#self-test-cancel").disabled = !ready || !status?.active || status.state === "cleanup"
    || pending || Boolean(snapshot.busy) || Boolean(snapshot.msc?.transition_pending)
    || Boolean(snapshot.msc_command_pending);
  size.disabled = !ready || Boolean(status?.active) || pending;
}