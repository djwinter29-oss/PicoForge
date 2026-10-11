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
  document.querySelector("#clock").textContent = runtime
    ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--";
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
  renderMemory(snapshot.memory);

  const updatedAt = snapshot.updated_at ? new Date(snapshot.updated_at).toLocaleTimeString() : null;
  document.querySelector("#sample-time").textContent = updatedAt ? `Last report ${updatedAt}` : "Waiting for HID report";
  const message = snapshot.error ?? (runtime ? "HID status is live" : connected ? "Waiting for first status report" : "Connect a PicoForge board");
  const statusLine = document.querySelector(".status-line");
  statusLine.dataset.error = String(Boolean(snapshot.error));
  statusLine.dataset.active = String(Boolean(connected && runtime));
  document.querySelector("#status-message").textContent = message;
}

function renderMemory(memory) {
  const set = (id, value) => { document.querySelector(id).textContent = value; };
  const manufacturer = memory?.manufacturer;
  set("#memory-presence", !memory?.configured ? "Not configured"
    : memory.presence_known ? (memory.present ? "Present" : "Absent") : "Unknown");
  set("#memory-type", memory?.component_type ?? "--");
  set("#memory-mode", memory?.mode?.toUpperCase() ?? "--");
  set("#memory-speed", memory?.bus_clock_hz
    ? `${(memory.bus_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz`
    : "--");
  set("#memory-size", memory?.capacity_bytes ? formatBytes(memory.capacity_bytes) : "--");
  set("#memory-manufacturer", manufacturer ? `0x${manufacturer.id.toString(16).padStart(2, "0")} / ${manufacturer.oem_id}` : "--");
  set("#memory-product", manufacturer ? `${manufacturer.product_name} rev ${manufacturer.product_revision}` : "--");
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