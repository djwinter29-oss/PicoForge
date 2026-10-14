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
  const clock = runtime
    ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--";
  document.querySelector("#identity-clock").textContent = clock;
  document.querySelector("#runtime-clock").textContent = clock;
  document.querySelector("#serial").textContent = hardware?.serial_number || "--";
  document.querySelector("#uptime").textContent = runtime ? uptimeLabel(runtime.uptime_ms) : "--";
  setState("#cdc-state", runtime ? (runtime.cdc_open ? "Open" : "Closed") : "--", runtime?.cdc_open);
  setState("#usb-state", runtime ? (runtime.usb_mounted ? "Mounted" : "Not mounted") : "--", runtime?.usb_mounted);
  document.querySelector("#sequence").textContent = runtime?.sequence ?? "--";
  const board = snapshot.board_status;
  document.querySelector("#temperature").textContent = board?.temperature_valid
    ? `${(board.temperature_centidegrees / 100).toFixed(2)} °C` : "--";
  document.querySelector("#led-state").textContent = board?.led_available
    ? (board.led_on ? "On" : "Off") : "Unavailable";
  document.querySelector("#led-control").textContent = board?.led_on ? "Turn LED off" : "Turn LED on";
  const msc = snapshot.msc;
  const mscControl = document.querySelector("#msc-control");
  mscControl.textContent = msc?.enabled ? "Disable USB drive" : "Enable USB drive";
  mscControl.disabled = !connected || !msc || Boolean(msc.transition_pending)
    || Boolean(snapshot.self_test?.active) || Boolean(snapshot.self_test_command_pending);
  document.querySelector("#msc-note").textContent = msc?.request_rejected
    ? "Request refused. Safely eject the drive on the host before disabling."
    : msc?.transition_pending ? "USB mode changing; the device will reconnect."
      : msc?.enabled ? "Host owns the card. Safely eject before disabling or using it in firmware."
        : "Firmware owns the card. Unmount firmware storage before enabling; the device then reconnects.";
  renderMemory(snapshot.memory, snapshot.filesystem_format);
  renderSelfTest(snapshot);

  const updatedAt = snapshot.updated_at ? new Date(snapshot.updated_at).toLocaleTimeString() : null;
  document.querySelector("#sample-time").textContent = updatedAt ? `Last report ${updatedAt}` : "Waiting for HID report";
  const message = snapshot.error ?? (runtime ? "HID status is live" : connected ? "Waiting for first status report" : "Connect a PicoForge board");
  const statusLine = document.querySelector(".status-line");
  statusLine.dataset.error = String(Boolean(snapshot.error));
  statusLine.dataset.active = String(Boolean(connected && runtime));
  document.querySelector("#status-message").textContent = message;
}

function renderMemory(memory, filesystemFormat) {
  const set = (id, value) => { document.querySelector(id).textContent = value; };
  const manufacturer = memory?.manufacturer;
  set("#memory-presence", !memory?.configured ? "Not configured"
    : memory.presence_known ? (memory.present ? "Present" : "Absent") : "Unknown");
  set("#memory-state", memory?.initialized ? "Ready" : memory?.configured ? "Not initialized" : "Unavailable");
  set("#memory-note", !memory?.configured ? "No SD-card transport configured"
    : memory.presence_known && !memory.present
      ? "No card detected. Check card insertion and the active-high DET connection on GP8."
      : memory.present && !memory.initialized
        ? "Card detected but not initialized; check UART logs and SD wiring."
        : memory.initialized ? "Card metadata is available." : "Card presence has not been established.");
  set("#memory-type", memory?.component_type ?? "--");
  set("#memory-mode", memory?.mode?.toUpperCase() ?? "--");
  set("#memory-speed", memory?.bus_clock_hz
    ? `${(memory.bus_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--");
  set("#memory-size", memory?.capacity_bytes ? formatBytes(memory.capacity_bytes) : "--");
  const filesystemLabels = { unknown: "Unknown", fat12: "FAT12", fat16: "FAT16", fat32: "FAT32", exfat: "exFAT" };
  set("#memory-filesystem", filesystemLabels[filesystemFormat] ?? "Unknown");
  set("#memory-manufacturer", manufacturer ? `0x${(manufacturer.id ?? manufacturer.manufacturer_id).toString(16).padStart(2, "0")} / ${manufacturer.oem_id}` : "--");
  set("#memory-product", manufacturer
    ? `${manufacturer.product_name} rev ${manufacturer.product_revision} · ${manufacturer.manufacture_year}-${String(manufacturer.manufacture_month).padStart(2, "0")}`
    : "--");
  set("#memory-serial", manufacturer ? `0x${manufacturer.serial_number.toString(16).padStart(8, "0")}` : "--");
}

function formatBytes(bytes) {
  const gibibytes = bytes / (1024 ** 3);
  return gibibytes >= 1 ? `${gibibytes.toFixed(2)} GiB` : `${(bytes / (1024 ** 2)).toFixed(0)} MiB`;
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
  if (status?.volume_mounted) notices.push("Filesystem remains mounted.");
  if (status?.mismatch_count) notices.push(`${status.mismatch_count} mismatched bytes.`);
  set("#self-test-message", notices.join(" ") || (status?.state === "passed"
    ? "Every byte verified; temporary file removed."
    : status?.state === "cancelled" ? "Cancelled; cleanup completed." : "No completed result."));
  const pending = Boolean(snapshot.self_test_command_pending);
  const mounted = ["fat12", "fat16", "fat32", "exfat"].includes(snapshot.filesystem_format);
  const ready = snapshot.connected && snapshot.memory?.initialized && mounted && status && !snapshot.msc?.enabled;
  const size = document.querySelector("#self-test-size");
  const validSize = Number.isInteger(size.valueAsNumber) && size.valueAsNumber >= 1 && size.valueAsNumber <= 128;
  document.querySelector("#self-test-start").disabled = !ready || status.active || pending || Boolean(snapshot.busy)
    || !validSize || !document.querySelector("#self-test-consent").checked;
  document.querySelector("#self-test-cancel").disabled = !snapshot.connected || !status?.active || status.state === "cleanup" || pending || Boolean(snapshot.busy);
  size.disabled = !snapshot.connected || Boolean(status?.active) || pending;
}