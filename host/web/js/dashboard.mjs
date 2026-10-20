function uptimeLabel(milliseconds) {
  if (!Number.isFinite(milliseconds)) return "--";
  const totalSeconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  return days ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${seconds}s`;
}

export function renderStatus(snapshot) {
  const connected = Boolean(snapshot.connected);
  const hardware = snapshot.hardware;
  const runtime = snapshot.runtime;
  const connection = document.querySelector("#connection");
  connection.dataset.state = snapshot.error ? "error" : connected ? "connected" : "disconnected";
  document.querySelector("#connection-label").textContent = snapshot.error
    ? "Attention" : connected ? "Connected" : "Disconnected";
  document.querySelector("#product-name").textContent = hardware?.product_name ?? "--";
  document.querySelector("#mcu").textContent = hardware?.mcu ?? "--";
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
  const ramUsed = runtime?.ram_used_bytes;
  const ramTotal = runtime?.ram_total_bytes;
  document.querySelector("#ram-reserved").textContent = runtime?.ram_total_bytes
    ? `${(ramUsed / 1024).toFixed(1)} KiB (${(ramUsed * 100 / ramTotal).toFixed(1)}%)`
    : "--";
  document.querySelector("#ram-total").textContent = runtime?.ram_total_bytes
    ? `${(ramTotal / 1024).toFixed(0)} KiB`
    : "--";
  document.querySelector("#serial").textContent = hardware?.serial_number || "--";
  document.querySelector("#uptime").textContent = runtime ? uptimeLabel(runtime.uptime_ms) : "--";
  setState("#cdc-state", runtime ? (runtime.cdc_open ? "Open" : "Closed") : "--", runtime?.cdc_open);
  const memory = snapshot.memory;
  const cardAbsent = memory?.presence_known === true && memory.present === false;
  const storageShared = snapshot.msc?.enabled || snapshot.msc?.transition_pending;
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
  mscControl.disabled = snapshot.settings_busy || !connected || !msc || Boolean(msc.transition_pending)
    || Boolean(snapshot.self_test?.active) || Boolean(snapshot.self_test_command_pending)
    || Boolean(msc?.enabled && !msc.force_return_supported);
  document.querySelector("#msc-note").textContent = msc?.enabled && !msc.force_return_supported
    ? "Update firmware to disable the USB drive from Pico; this firmware requires manual host eject."
    : msc?.request_rejected
    ? (msc.enabled ? "Request refused. Disabling requires idle storage and successful device sync. Check diagnostics."
      : "Storage return failed. Volume may be unmounted; inspect media and UART errors before mounting.")
    : msc?.transition_pending ? "Preparing storage and changing USB mode; the device will reconnect."
      : "";
  document.querySelector("#msc-note").hidden = !document.querySelector("#msc-note").textContent;
  renderMemory(snapshot.memory, snapshot.filesystem_format);
  renderSelfTest(snapshot);

  const message = snapshot.error ?? (runtime ? "HID status is live"
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
  const storageBusy = !msc || msc.enabled || msc.transition_pending
    || snapshot.self_test?.active || snapshot.self_test_command_pending;
  const ready = supported && !storageBusy;
  const storageControl = document.querySelector("#settings-storage");
  updateClockOptions(document.querySelector("#settings-clock"), snapshot.hardware?.mcu,
    settings?.system_clock_mhz);
  if (settings && !snapshot.settings_dirty) {
    document.querySelector("#settings-clock").value = String(settings.system_clock_mhz);
  }
  if (settings && !snapshot.settings_storage_dirty) {
    storageControl.value = String(storageSpeed(settings, snapshot.memory?.mode));
  }
  for (const selector of ["#settings-clock", "#settings-storage"]) {
    document.querySelector(selector).disabled = snapshot.settings_busy || !ready;
  }
  document.querySelector("#settings-save").disabled = snapshot.settings_busy || !ready || !snapshot.settings_dirty;
  document.querySelector("#settings-note").textContent = snapshot.settings_message
    || (!snapshot.connected ? "Waiting for a PicoForge connection."
      : !snapshot.settings_supported ? "Boot settings unavailable; update device firmware."
        : !msc ? "Waiting for storage ownership status."
          : msc.enabled || msc.transition_pending ? "Disable the USB drive before saving settings."
            : snapshot.self_test?.active || snapshot.self_test_command_pending
              ? "Wait for the storage test to finish before saving settings."
              : snapshot.settings_dirty ? "Unsaved changes."
                : "Settings loaded.");
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

export function storageSpeed(settings, transport) {
  if (transport === "sdio") return settings.sdio_bus_mhz === 50 ? 50 : 25;
  if (transport === "spi") return settings.spi_bus_mhz === 25 ? 25 : 50;
  return settings.sdio_bus_mhz === 50 || settings.spi_bus_mhz === 50 ? 50 : 25;
}

function renderMemory(memory, filesystemFormat) {
  const set = (id, value) => { document.querySelector(id).textContent = value; };
  const manufacturer = memory?.manufacturer;
  set("#memory-state", memory?.initialized ? "Ready" : memory?.configured ? "Not initialized" : "Unavailable");
  set("#memory-note", !memory?.configured ? "No SD-card transport configured"
    : memory.presence_known && !memory.present
      ? "No card detected. Check card insertion and the active-high DET connection on GP8."
      : memory.present && !memory.initialized
        ? "Card detected but not initialized; check UART logs and SD wiring."
        : memory.initialized ? "" : "Card presence has not been established.");
  set("#memory-mode", memory?.mode?.toUpperCase() ?? "--");
  set("#memory-speed", memory?.bus_clock_hz
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
  set("#self-test-state", labels[status?.state] ?? "Unavailable");
  set("#self-test-percent", `${Math.floor(status?.progress_percent ?? 0)}%`);
  document.querySelector("#self-test-progress").value = status?.progress_percent ?? 0;
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
    || !validSize || Boolean(snapshot.msc?.transition_pending);
  document.querySelector("#self-test-cancel").disabled = !snapshot.connected || !status?.active || status.state === "cleanup" || pending || Boolean(snapshot.busy);
  size.disabled = !snapshot.connected || Boolean(status?.active) || pending;
}