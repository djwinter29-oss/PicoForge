/**
 * Same-tab recovery records for an uncertain device write. Records hold only allowlisted transaction, profile,
 * commit-mask or generation fields, never credentials; anything else fails closed.
 */
import { validSettings } from "./protocol.mjs?v=47";

const PENDING_OPERATION_KEY = "picoforge.direct.pending-operation";
const isUid = value => typeof value === "string" && /^[A-F0-9]{16}$/i.test(value);
const hasExactKeys = (value, keys) => value && typeof value === "object" && !Array.isArray(value)
  && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const inRange = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;

/** @brief Validate one version-2 record exactly; @return the record, or throw. */
export function validatePendingOperation(record) {
  if (!record || record.version !== 2 || !isUid(record.uid)) throw new Error("Invalid saved operation identity");
  if (record.kind === "control") {
    const request = record.request;
    const requestKeys = request?.opcode === 5 ? ["transaction_id", "opcode", "settings", "commit_mask"]
      : request?.opcode === 12 ? ["transaction_id", "opcode", "scan_generation", "scan_index"]
        : request?.lifecycle ? ["transaction_id", "opcode", "lifecycle"] : ["transaction_id", "opcode"];
    const settingsKeys = ["system_clock_mhz", "sdcard_profile", "radios", "tcp_cli_enabled"];
    if (!hasExactKeys(record, ["version", "kind", "uid", "request"])
      || !hasExactKeys(request, requestKeys) || !inRange(request.transaction_id, 1, 255)
      || !inRange(request.opcode, 1, 13)
      || request.lifecycle && (!hasExactKeys(request.lifecycle, ["operation", "generation"])
        || request.lifecycle.operation !== request.opcode || ![9, 11].includes(request.opcode)
        || !inRange(request.lifecycle.generation, 1, request.opcode === 9 ? 255 : 65535))
      // Exact keys require a stored radios value, so the shared settings rule cannot accept an omitted one.
      || request.opcode === 5 && (!hasExactKeys(request.settings, settingsKeys)
        || !validSettings(request.settings) || !inRange(request.commit_mask, 0, 7))
      || request.opcode === 12 && (!inRange(request.scan_generation, 1, 65535)
        || !inRange(request.scan_index, 0, 17))) {
      throw new Error("Invalid saved control transaction");
    }
  } else if (record.kind === "wireless-lifecycle") {
    if (!hasExactKeys(record, ["version", "kind", "uid", "operation", "generation"])
      || ![9, 11].includes(record.operation)
      || !inRange(record.generation, 1, record.operation === 9 ? 255 : 65535)) {
      throw new Error("Invalid saved wireless lifecycle");
    }
  } else throw new Error("Unknown saved operation kind");
  return record;
}

/** @brief Default store: this tab's sessionStorage, or null when the browser denies it. */
export function defaultPendingStore() {
  try { return globalThis.sessionStorage ?? null; } catch { return null; }
}

/** @brief Read the saved record; @return null when none is saved; throw on unavailable or invalid data. */
export function loadPendingOperation(store) {
  if (!store) throw new Error("session storage is unavailable");
  const raw = store.getItem(PENDING_OPERATION_KEY);
  return raw ? validatePendingOperation(JSON.parse(raw)) : null;
}

/** @brief Save a validated record, or clear the slot when record is null. Throws when storage is unavailable. */
export function savePendingOperation(store, record) {
  if (!store) throw new Error("Session storage is unavailable; refusing an untracked device write.");
  if (record) store.setItem(PENDING_OPERATION_KEY, JSON.stringify(validatePendingOperation(record)));
  else store.removeItem(PENDING_OPERATION_KEY);
}
