// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file Single-flight control command queue with a same-tab recovery journal. Every device write is journalled
 * before it is sent; an outcome that is not known stays persisted and is never replayed. Records hold only the
 * descriptor-declared request facts (protocol/opcodes.mjs), never credentials; anything else fails closed.
 */
import { buildControlRequest, decodeControlResult } from "../protocol/codec.mjs";
import { OP, REPORT, RESULT } from "../protocol/constants.mjs";
import { allowedOnLink, OPCODES } from "../protocol/opcodes.mjs";
import { BoardError, errorFromValue, errorValue, REASON } from "../state/errors.mjs";
import { commandBlocked, commandPending } from "../state/selectors.mjs";
import { withTimeout } from "../timers.mjs";

const PENDING_OPERATION_KEY = "picoforge.direct.pending-operation";
/**
 * @typedef {{flags?: number, args?: ArrayLike<number>, signal?: AbortSignal,
 *   lifecycle?: import("../state/reducer.mjs").WirelessLifecycle,
 *   commitMask?: number, workflow?: symbol}} CommandOptions
 */
const isUid = value => typeof value === "string" && /^[A-F0-9]{16}$/i.test(value);
const hasExactKeys = (value, keys) => value && typeof value === "object" && !Array.isArray(value)
  && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const inRange = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;
const validLifecycle = (lifecycle, opcode) => hasExactKeys(lifecycle, ["operation", "generation"])
  && lifecycle.operation === opcode && inRange(lifecycle.generation, 1, OPCODES[opcode]?.lifecycle?.generationMax);
/** @brief Failure code when a completed result does not prove the request; default NOT_CONFIRMED. */
const UNCONFIRMED = { [OP.SCAN_ENTRY]: REASON.SCAN_MISMATCH };

/** @brief A refused or failed final result; recovery omits the detail byte. */
export const commandFailed = (result, withDetail = true) => new BoardError(REASON.COMMAND_FAILED, {
  opcode: result.opcode, status: result.status, error: result.error_reason,
  ...withDetail ? { detail: result.detail } : {} });

/**
 * @brief Validate one version-2 journal record exactly; @return the record, or throw.
 * @param {import("../state/reducer.mjs").PendingOperation} record
 */
export function validatePendingOperation(record) {
  if (!record || record.version !== 2 || !isUid(record.uid)) throw new BoardError(REASON.JOURNAL_IDENTITY);
  if (record.kind === "control") {
    const request = record.request;
    const descriptor = OPCODES[request?.opcode];
    const facts = descriptor?.facts?.keys ?? (request?.lifecycle ? ["lifecycle"] : []);
    if (!descriptor || !hasExactKeys(record, ["version", "kind", "uid", "request"])
      || !hasExactKeys(request, ["transaction_id", "opcode", ...facts]) || !inRange(request.transaction_id, 1, 255)
      || request.lifecycle && !validLifecycle(request.lifecycle, request.opcode)
      || descriptor.facts && !descriptor.facts.valid(request)) {
      throw new BoardError(REASON.JOURNAL_TRANSACTION);
    }
  } else if (record.kind === "wireless-lifecycle") {
    if (!hasExactKeys(record, ["version", "kind", "uid", "operation", "generation"])
      || !validLifecycle({ operation: record.operation, generation: record.generation }, record.operation)) {
      throw new BoardError(REASON.JOURNAL_LIFECYCLE);
    }
  } else throw new BoardError(REASON.JOURNAL_KIND);
  return record;
}

/** @brief Default journal store: this tab's sessionStorage, or null when the browser denies it. */
export function defaultPendingStore() {
  try { return globalThis.sessionStorage ?? null; } catch { return null; }
}

/** @brief Read the saved record; @return null when none is saved; throw on unavailable or invalid data. */
export function loadPendingOperation(store) {
  if (!store) throw new BoardError(REASON.SESSION_STORAGE);
  const raw = store.getItem(PENDING_OPERATION_KEY);
  return raw ? validatePendingOperation(JSON.parse(raw)) : null;
}

/** @brief Save a validated record, or clear the slot when record is null. Throws when storage is unavailable. */
export function savePendingOperation(store, record) {
  if (!store) throw new BoardError(REASON.STORAGE_UNAVAILABLE);
  if (record) store.setItem(PENDING_OPERATION_KEY, JSON.stringify(validatePendingOperation(record)));
  else store.removeItem(PENDING_OPERATION_KEY);
}

/** @brief Command queue, journal and result matching for one board session. */
export class Commands {
  /**
   * @param {import("./session.mjs").BoardSession} session Facade giving access to the transport, store and peers.
   * @param pendingStore Journal storage (sessionStorage-like), or null when unavailable.
   */
  constructor(session, pendingStore) {
    this.session = session;
    this.pendingStore = pendingStore;
    /** Last transaction ID sent (1..255), restored from the journal. */
    this.transaction = 0;
    /** `{opcode, signal}` while a command owns the queue. */
    this.queue = null;
    /** Forced save awaiting reconnect recovery: `{done, settle, outcome}`. */
    this.forcedSave = null;
    /** State error value when the journal could not be read; every write stays uncertain until reload. */
    this.recoveryError = null;
    /** Journal facts restored at construction: `{uid, request, wireless}`. */
    /** @type {{uid: string | null, request: import("../state/reducer.mjs").PendingRequest | null,
     *   wireless: import("../state/reducer.mjs").WirelessLifecycle | null}} */
    this.restored = { uid: null, request: null, wireless: null };
    try {
      const record = loadPendingOperation(pendingStore);
      if (record) {
        this.transaction = record.kind === "control" ? record.request.transaction_id : 0;
        this.restored = { uid: record.uid, request: record.kind === "control" ? record.request : null,
          wireless: record.kind === "wireless-lifecycle"
            ? { operation: record.operation, generation: record.generation } : null };
      }
    } catch (error) {
      this.recoveryError = { code: REASON.RECOVERY_UNAVAILABLE, detail: errorValue(error) };
    }
  }

  /** @brief Persist the journal record for the given pending facts before they enter state. */
  persist(request, wireless) {
    const s = this.session;
    const uid = s.state.hardware?.serial_number ?? s.connection.reconnectUid;
    savePendingOperation(this.pendingStore, request ? { version: 2, kind: "control", uid, request }
      : wireless ? { version: 2, kind: "wireless-lifecycle", uid, ...wireless } : null);
  }

  /**
   * @brief Retire the outstanding transaction. A successful lifecycle request becomes the pending wireless command;
   * a failed MSC request releases its pending ownership change.
   */
  retire(request, succeeded) {
    const wireless = succeeded && request.lifecycle ? request.lifecycle : null;
    this.persist(null, wireless);
    this.session.emit("command/retired", { pending_request: null, pending_wireless: wireless,
      ...(!succeeded && request.opcode === OP.MSC ? { pending_msc: null } : {}) });
  }

  /** @brief Clear the journal after a wireless lifecycle outcome is known. */
  settleWireless() {
    this.persist(null, null);
    this.session.emit("lifecycle/settled", { pending_wireless: null });
  }

  /** @brief Hand a recovered result (or recovery failure) to the forced save waiting for it. */
  settleForcedSave(outcome) { this.forcedSave?.settle(outcome); }

  /** @brief Failure code when a completed result does not prove the request's effect, else null. */
  unconfirmed(request, result) {
    const confirms = OPCODES[request.opcode]?.confirms;
    return !confirms || confirms(result, request) ? null : UNCONFIRMED[request.opcode] ?? REASON.NOT_CONFIRMED;
  }

  /**
   * @brief Send one control request and wait for its authoritative result; the outcome stays journalled until known.
   * A successful command whose descriptor `reboots` marks the board as rebooting.
   * @param {number} opcode
   * @param {CommandOptions} [options]
   */
  async command(opcode, options = {}) {
    const s = this.session;
    if (!allowedOnLink(s.state.link, opcode, options.flags ?? 0)) throw new BoardError(REASON.USB_ONLY);
    if (this.queue) throw new BoardError(REASON.QUEUE_BUSY);
    const args = options.args ? Array.from(options.args) : undefined;
    const signal = options.signal ? AbortSignal.any([s.connectionSignal, options.signal]) : s.connectionSignal;
    this.queue = { opcode, signal };
    try {
      const result = await this.execute(opcode, { ...options, args, signal });
      if (OPCODES[opcode]?.reboots) s.emit("command/finished", { reboot_pending: true });
      return result;
    } finally {
      args?.fill(0);
      this.queue = null;
    }
  }

  async execute(opcode, options) {
    const s = this.session;
    s.polling.expireReports();
    const signal = options.signal;
    if (signal.aborted) throw signal.reason ?? new DOMException(REASON.OPERATION_CANCELLED, "AbortError");
    if (!s.device || commandBlocked(s.state)
      || s.state.wireless_busy && options.workflow !== s.operations.wirelessOperation) {
      throw new BoardError(REASON.COMMAND_BLOCKED);
    }
    const descriptor = OPCODES[opcode];
    const bit = descriptor?.capability ?? 0;
    if (bit && !(s.state.hardware.capabilities & bit)) throw new BoardError(REASON.CAPABILITY_UNAVAILABLE);
    const transaction = this.transaction % 255 + 1;
    const report = buildControlRequest(transaction, opcode, options);
    // A link-resetting request (forced save) is settled by reconnect recovery after the same board returns.
    const forced = descriptor.resetsLink?.(report) ? {} : null;
    if (forced) forced.done = new Promise(resolve => { forced.settle = outcome => resolve(forced.outcome = outcome); });
    this.forcedSave = forced;
    this.transaction = transaction;
    /** @type {import("../state/reducer.mjs").PendingRequest} */
    const request = { transaction_id: transaction, opcode, ...descriptor.facts?.read(report, options) };
    if (options.lifecycle) request.lifecycle = options.lifecycle;
    const device = s.device;
    const generation = s.connection.generation;
    const current = () => generation === s.connection.generation;
    this.persist(request, null);
    s.emit("command/started", { busy: true, pending_request: request, error: null, recovery_message: null,
      recovered_opcode: null, ...(opcode === OP.MSC ? { pending_msc: report[4] === 1 } : {}) });
    let sent = false;
    try {
      await withTimeout(s.transport.writeReport(REPORT.CONTROL, report, signal), s.timeout, signal, s.timers);
      sent = true;
      if (!current()) throw new BoardError(REASON.OUTCOME_UNCERTAIN);
      const result = await this.pollResult(device, request, signal);
      if (result.status !== RESULT.COMPLETED) {
        this.retire(request, false);
        throw commandFailed(result);
      }
      const unconfirmed = this.unconfirmed(request, result);
      if (unconfirmed) throw new BoardError(unconfirmed);
      if (result.settings) s.emit("command/finished", { settings: result.settings });
      this.retire(request, true);
      return result;
    } catch (error) {
      if (forced && sent && s.connection.reconnectUid && !(current() && s.state.outcome_uncertain)
        && (s.state.pending_request || forced.outcome)) {
        const { result } = forced.outcome ?? await withTimeout(forced.done, 5000, undefined, s.timers)
          .catch(() => ({}));
        if (result?.transaction_id === transaction && result.opcode === opcode) {
          if (result.status !== RESULT.COMPLETED) throw commandFailed(result, false);
          if (!this.unconfirmed(request, result)) {
            s.emit("command/finished", { settings: result.settings, outcome_uncertain: false });
            return result;
          }
        }
        if (s.state.error) throw errorFromValue(s.state.error);
      }
      // SET rejection is authoritative: do not poll or accept a prior result.
      if (!sent && !["TimeoutError", "AbortError"].includes(error.name) && current()) {
        try { this.retire(request, false); }
        catch (storageError) {
          s.emit("session/failed", { error: { code: REASON.RECOVERY_CLEAR_FAILED, detail: errorValue(storageError) } });
        }
      }
      if (current()) {
        s.emit("command/finished", { outcome_uncertain: commandPending(s.state), error: errorValue(error) });
      }
      throw error;
    } finally {
      this.forcedSave = null;
      report.fill(0);
      if (current()) s.emit("command/finished", { busy: false });
    }
  }

  /** @brief Poll Feature 20 until the request's final result; any connection change leaves the outcome uncertain. */
  async pollResult(device, request, signal = this.session.connectionSignal) {
    const s = this.session;
    const deadline = s.now() + s.timeout;
    const generation = s.connection.generation;
    for (let attempt = 0; attempt < Math.ceil(s.timeout / 50) && s.now() < deadline; attempt++) {
      if (signal.aborted || device !== s.device) throw new BoardError(REASON.OUTCOME_UNCERTAIN);
      const report = await withTimeout(s.transport.readReport(REPORT.CONTROL, signal), deadline - s.now(),
        signal, s.timers);
      if (signal.aborted || device !== s.device || generation !== s.connection.generation) {
        throw new BoardError(REASON.OUTCOME_UNCERTAIN);
      }
      const result = decodeControlResult(report, { ble: s.state.link === "ble" });
      if (result.transaction_id === request.transaction_id && result.opcode === request.opcode
        && result.status > RESULT.PENDING) return result;
      await s.sleep(50, signal);
    }
    throw new BoardError(REASON.RESULT_TIMEOUT);
  }
}
