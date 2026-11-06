import { CAP } from "../../protocol/constants.mjs";
import { mscPending } from "../../state/selectors.mjs";
import { queryElement, setText } from "../dom.mjs";

function formatBytes(bytes) {
  const gibibytes = bytes / 1024 ** 3;
  return gibibytes >= 1 ? `${gibibytes.toFixed(2)} GiB`
    : bytes < 1048576 ? `${(bytes / 1024).toFixed(1)} KiB` : `${(bytes / 1024 ** 2).toFixed(2)} MiB`;
}

function formatManufacturer(manufacturer) {
  if (!manufacturer) return "--";
  const id = manufacturer.id;
  if (!Number.isInteger(id) || id < 0 || id > 0xff) return "Unknown manufacturer";
  const names = { 0x03: "SanDisk", 0x41: "Kingston" };
  return `${names[id] ?? "Unknown manufacturer"} (0x${id.toString(16).padStart(2, "0")})`;
}

/** Retained card details are display-only; they never participate in operation eligibility. */
export function deriveStorageViewModel(state, previous = {}) {
  let memory = state.memory;
  let filesystemFormat = state.filesystem_format;
  const msc = state.msc;
  const absent = memory?.presence_known === true ? memory.present === false
    : state.connected ? previous.absent ?? false : false;
  const busy = memory?.configured && memory.sample_valid === false && !msc?.enabled && !msc?.transition_pending;
  const details = memory?.sample_valid && memory.initialized && !msc?.enabled && !msc?.transition_pending
    ? { memory, filesystemFormat } : busy && state.connected ? previous.details : null;
  const label = msc?.enabled ? "USB host" : msc?.transition_pending ? "Transitioning" : busy ? "Busy"
    : memory?.initialized ? "Ready" : memory?.configured ? "Not initialized" : "Unavailable";
  const note = !memory ? "Waiting for fresh storage status."
    : !memory.sample_valid ? "" : !memory.configured ? "No SD-card transport configured"
      : memory.presence_known && !memory.present
        ? "No card detected. Check card insertion and the configured card-detect connection."
        : memory.present && !memory.initialized ? "Card detected but not initialized; check UART logs and SD wiring."
          : memory.initialized ? "" : "Card presence has not been established.";
  if (busy && details) ({ memory, filesystemFormat } = details);
  const manufacturer = memory?.manufacturer;
  const filesystemLabels = { unknown: "Unknown", fat12: "FAT12", fat16: "FAT16", fat32: "FAT32", exfat: "exFAT" };
  return {
    retained: { absent, details },
    hidden: absent,
    selfTestHidden: absent || Boolean(msc?.enabled || msc?.transition_pending || mscPending(state))
      || !(state.hardware?.capabilities & CAP.SELF_TEST),
    text: {
      "memory-state": label,
      "memory-note": note,
      "memory-speed": memory?.sample_valid !== false && memory?.initialized && !msc?.enabled
        && !msc?.transition_pending && memory?.bus_clock_hz
        ? `${(memory.bus_clock_hz / 1_000_000).toLocaleString(undefined, { maximumFractionDigits: 3 })} MHz` : "--",
      "memory-size": memory?.capacity_bytes ? formatBytes(memory.capacity_bytes) : "--",
      "memory-filesystem": filesystemLabels[filesystemFormat] ?? "Unknown",
      "memory-manufacturer": formatManufacturer(manufacturer),
      "memory-product": manufacturer
        ? `${manufacturer.product_name} rev ${manufacturer.product_revision} · ${manufacturer.manufacture_year}-`
          + String(manufacturer.manufacture_month).padStart(2, "0") : "--",
      "memory-serial": manufacturer ? manufacturer.serial_number.toString(16).padStart(8, "0") : "--",
    },
  };
}

export function createStorageSection() {
  let previous = {};
  return state => {
    const model = deriveStorageViewModel(state, state.connected ? previous : {});
    previous = model.retained;
    setText(model.text);
    queryElement("#memory-note").hidden = !model.text["memory-note"];
    queryElement('[aria-labelledby="memory-title"]').hidden = model.hidden;
    queryElement('[aria-labelledby="self-test-title"]').hidden = model.selfTestHidden;
  };
}
