import { CAP } from "../../protocol/constants.mjs";
import { queryElement, setText } from "../dom.mjs";
import { deriveDashboardViewModel } from "./status-model.mjs";

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
  const hours = Math.floor(seconds % 86400 / 3600);
  const minutes = Math.floor(seconds % 3600 / 60);
  return days > 0 ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${seconds % 60}s`;
}

export function deriveDeviceViewModel(state) {
  const hardware = state.hardware;
  const runtime = state.runtime;
  const config = state.settings;
  const spi = hardware?.transport === "spi";
  const ramUsed = hardware?.ram_reserved_bytes;
  const ramTotal = hardware?.ram_total_bytes;
  const led = runtime?.led_available ? Boolean(runtime.led_on) : null;
  const signals = spi ? ["CLK", "MOSI", "MISO", "CS"] : ["CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3"];
  const pin = value => !config ? "--" : value === 255 || value == null ? "Unassigned" : `GP${value}`;
  return {
    ...deriveDashboardViewModel(state),
    active: Boolean(state.connected && runtime),
    error: Boolean(state.error),
    cdcActive: runtime?.cdc_open,
    led, ledState: led === null ? "unknown" : led ? "on" : "off",
    ledTitle: led === null ? "Onboard LED unavailable" : `Turn onboard LED ${led ? "off" : "on"}`,
    freshness: [[3, "telemetry"], [10, "storage"], [11, "self-test"]].map(([id, label]) => ({
      label, hidden: state.freshness?.[id] === "fresh",
      text: id === 11 && !(hardware?.capabilities & CAP.SELF_TEST) ? "Self-test unavailable"
        : `${label}: ${state.freshness?.[id] ?? "unavailable"}`,
    })),
    pins: {
      signature: JSON.stringify([spi, config?.monitor_uart_index, config?.monitor_uart_tx_gpio,
        config?.monitor_uart_rx_gpio, config?.storage_gpios, config?.card_detect_gpio, config?.reset_cause]),
      uart: [["TX", "RX"], [pin(config?.monitor_uart_tx_gpio), pin(config?.monitor_uart_rx_gpio)]],
      storage: [[...signals, "Card detect"],
        [...signals.map((_, index) => pin(config?.storage_gpios?.[index])), pin(config?.card_detect_gpio)]],
    },
    text: {
      "product-name": hardware ? `${hardware.product_name} (${spi ? "SPI" : "SDIO"})` : "--",
      "board-model": hardware ? `${hardware.board_name} (${hardware.mcu})` : "--",
      "firmware-version": hardware?.firmware_version ?? "Unknown",
      "identity-clock": runtime
        ? `${(runtime.system_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz` : "--",
      "cpu-usage-core0": runtime?.cpu_usage_core0_basis_points == null
        ? "--" : `${(runtime.cpu_usage_core0_basis_points / 100).toFixed(1)}%`,
      "cpu-usage-core1": runtime?.cpu_usage_core1_basis_points == null
        ? "--" : `${(runtime.cpu_usage_core1_basis_points / 100).toFixed(1)}%`,
      "ram-usage": Number.isSafeInteger(ramTotal) && ramTotal > 0 && Number.isSafeInteger(ramUsed)
        && ramUsed >= 0 && ramUsed <= ramTotal
        ? `${(ramUsed / 1024).toFixed(1)} / ${(ramTotal / 1024).toFixed(0)} KiB`
          + ` (${(ramUsed * 100 / ramTotal).toFixed(1)}%)` : "--",
      serial: hardware?.serial_number || "--",
      uptime: runtime ? uptimeLabel(runtime.uptime_ms) : "--",
      "cdc-state": runtime ? runtime.cdc_open ? "Open" : "Closed" : "--",
      temperature: runtime?.temperature_valid ? `${(runtime.temperature_centidegrees / 100).toFixed(2)} °C` : "--",
      "led-control": led === null ? "Board LED: --" : `Board LED: ${led ? "On" : "Off"}`,
      "board-uart-title": config && config.monitor_uart_index !== 255
        ? `Monitor UART${config.monitor_uart_index}` : "Monitor UART",
      "board-storage-title": config ? `Storage (${spi ? "SPI" : "SDIO-PIO"})` : "Storage",
      "board-reset-cause": config
        ? ["Unknown", "Power-on", "Watchdog", "Software request", "Watchdog after HardFault"][config.reset_cause]
          ?? `Unassigned (${config.reset_cause})` : "--",
    },
  };
}

export function renderDevice(state) {
  const model = deriveDeviceViewModel(state);
  setText({ ...model.text, "connection-label": model.connection_label, "status-message": model.status_message });
  queryElement("#connection").dataset.state = model.connection_state;
  const cdc = queryElement("#cdc-state");
  if (model.cdcActive === undefined) delete cdc.dataset.active;
  else cdc.dataset.active = String(model.cdcActive);
  const led = queryElement("#led-control");
  led.dataset.state = model.ledState;
  led.setAttribute("aria-pressed", String(model.led === true));
  led.title = model.ledTitle;
  queryElement(".status-line").dataset.error = String(model.error);
  queryElement(".status-line").dataset.active = String(model.active);
  for (const status of model.freshness) {
    const element = queryElement(`#${status.label}-freshness`);
    element.textContent = status.text;
    element.hidden = status.hidden;
  }
  const uart = queryElement("#board-uart");
  if (uart.dataset.boardConfiguration === model.pins.signature) return;
  const tables = /** @type {[string, [string[], string[]]][]} */ ([["board-uart", model.pins.uart],
    ["board-storage-pins", model.pins.storage]]);
  for (const [id, [labels, pins]] of tables) {
    const headings = document.createElement("tr");
    const values = document.createElement("tr");
    labels.forEach((label, index) => {
      const heading = document.createElement("th");
      heading.scope = "col";
      heading.textContent = label;
      const cell = document.createElement("td");
      cell.textContent = pins[index];
      headings.append(heading);
      values.append(cell);
    });
    queryElement(`#${id}`).replaceChildren(headings, values);
  }
  uart.dataset.boardConfiguration = model.pins.signature;
}
