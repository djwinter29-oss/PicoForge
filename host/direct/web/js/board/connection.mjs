/**
 * @file Connection lifecycle state machine: connect, same-board reconnect, initialization, recovery hand-off and
 * disconnect. The only writer of `phase` and its projections `connected` and `reconnecting`.
 */
import { decodeConfigReport, decodeIdentityReport } from "../protocol/codec.mjs";
import { CAP, REPORT } from "../protocol/constants.mjs";
import { BoardError, errorValue, REASON } from "../state/errors.mjs";
import { commandPending } from "../state/selectors.mjs";
import { withTimeout } from "../timers.mjs";
import { TRACKED_REPORTS } from "./polling.mjs";

/** @brief Connection phases. `connected` is true in RECOVERING and READY; `reconnecting` only in RECONNECTING. */
export const PHASE = Object.freeze({
  DISCONNECTED: "disconnected", /**< No link; nothing pending a reconnect. */
  CONNECTING: "connecting", /**< User-initiated transport open in progress. */
  RECONNECTING: "reconnecting", /**< Waiting for the same board to return so pending facts can be recovered. */
  INITIALIZING: "initializing", /**< Link open; reading identity and configuration. */
  RECOVERING: "recovering", /**< Board identified; settling journalled requests before user commands. */
  READY: "ready", /**< Board identified, recovery done, polling running. */
});

/**
 * @brief Forward edges of the lifecycle. Any phase may also stay where it is or close to DISCONNECTED or
 * RECONNECTING (link loss, failure or user disconnect).
 */
export const TRANSITIONS = Object.freeze({
  [PHASE.DISCONNECTED]: [PHASE.CONNECTING, PHASE.INITIALIZING],
  [PHASE.RECONNECTING]: [PHASE.INITIALIZING],
  [PHASE.CONNECTING]: [PHASE.INITIALIZING],
  [PHASE.INITIALIZING]: [PHASE.RECOVERING],
  [PHASE.RECOVERING]: [PHASE.READY],
  [PHASE.READY]: [],
});

/** @brief Whether the lifecycle allows moving from one phase to another. */
export const canTransition = (from, to) => from === to || to === PHASE.DISCONNECTED || to === PHASE.RECONNECTING
  || Boolean(TRANSITIONS[from]?.includes(to));

/** @brief Per-connection facts (state/reducer.mjs CONNECTION_FACTS) reset on connect and disconnect. */
/** @type {Readonly<Partial<import("../state/reducer.mjs").DashboardState>>} */
export const EMPTY = Object.freeze({
  hardware: null, settings: null, settings_supported: false,
  runtime: null, memory: null, filesystem_format: null, msc: null, self_test: null,
  wireless: null, wireless_error: null, wireless_poll_error: null, recovery_message: null, recovered_opcode: null,
  freshness: Object.fromEntries(TRACKED_REPORTS.map(id => [id, /** @type {const} */ ("unavailable")])),
  pending_self_test: null, reboot_pending: false,
});

export class Connection {
  /** @param {import("./session.mjs").BoardSession} session */
  constructor(session) {
    this.session = session;
    /** Incremented whenever the connection is cleared; fences every in-flight read and write. */
    this.generation = 0;
    /** Aborted whenever the connection is cleared. */
    this.controller = new AbortController();
    /** Serial number of the board that owns the journalled operation, or of the current board. */
    this.reconnectUid = null;
    const transport = session.transport;
    this.handlers = {
      report: event => session.polling.onInputReport(event),
      disconnect: () => this.clear(transport.autoReconnect && this.reconnectUid ? PHASE.RECONNECTING
        : PHASE.DISCONNECTED, { error: session.commands.recoveryError ?? (session.state.pending_request
        ? REASON.DISCONNECTED_UNCERTAIN : session.state.pending_wireless ? REASON.WIRELESS_DISCONNECTED : null) }),
      available: () => {
        if (this.reconnectUid && !session.device && !session.state.busy) void this.reconnect();
      },
      suspend: () => session.polling.expireReports(true),
    };
    for (const [type, handler] of Object.entries(this.handlers)) transport.addEventListener(type, handler);
  }

  get phase() { return this.session.state.phase; }
  get signal() { return this.controller.signal; }

  /** @brief Move to a phase, writing it and its projections in one event; throws on an illegal edge. */
  transition(to, changes = {}, type = "connection/changed") {
    if (!canTransition(this.phase, to)) throw new Error(`Illegal connection transition ${this.phase} -> ${to}`);
    this.session.emit(type, { ...changes, phase: to, connected: to === PHASE.RECOVERING || to === PHASE.READY,
      reconnecting: to === PHASE.RECONNECTING });
  }

  /** @brief Change connection flags (busy, error) without changing phase. */
  change(changes) { this.session.emit("connection/changed", changes); }

  /**
   * @brief Fence the old connection, stop polling and reset per-connection facts. Performs no device I/O.
   * @param {string} [phase] PHASE.DISCONNECTED or PHASE.RECONNECTING.
   * @param {object} [changes] Extra `connection/closed` facts, e.g. the error.
   */
  clear(phase = PHASE.DISCONNECTED, changes = {}) {
    const s = this.session;
    this.generation++;
    this.controller.abort(new DOMException(REASON.DEVICE_DISCONNECTED, "AbortError"));
    this.controller = new AbortController();
    s.polling.reset();
    this.transition(phase, { ...EMPTY, busy: false,
      outcome_uncertain: Boolean(s.commands.recoveryError || commandPending(s.state)), ...changes },
    "connection/closed");
  }

  /** @brief Open a user-selected board. */
  async connect(options) {
    const s = this.session;
    if (s.state.busy || s.device || s.state.reconnecting) return;
    const signal = this.signal;
    this.transition(PHASE.CONNECTING, { busy: true, error: s.commands.recoveryError });
    try {
      if (await s.transport.connect(options, signal)) await this.initialize();
    } catch (error) { if (!signal.aborted) this.settle({ error: errorValue(error) }); }
    finally { if (!signal.aborted) this.settle({ busy: false }); }
  }

  /** @brief Reopen the board that owns a pending operation after the transport reports it available. */
  async reconnect() {
    const signal = this.signal;
    this.change({ busy: true });
    try {
      if (await this.session.transport.reconnect(signal)) await this.initialize();
    } catch (error) { if (!signal.aborted) this.settle({ error: errorValue(error) }); }
    finally { if (!signal.aborted) this.settle({ busy: false }); }
  }

  /** @brief End a connect attempt that never reached initialization. */
  settle(changes) {
    if (this.phase === PHASE.CONNECTING) this.transition(PHASE.DISCONNECTED, changes);
    else this.change(changes);
  }

  /** @brief Identify the board, refuse a different board while a journalled operation exists, then recover. */
  async initialize() {
    const s = this.session;
    const device = s.device;
    const generation = this.generation;
    const signal = this.signal;
    const current = () => generation === this.generation && device === s.device;
    const read = async id => withTimeout(s.transport.readReport(id, signal), s.timeout, signal, s.timers);
    this.transition(PHASE.INITIALIZING, { busy: true });
    try {
      signal.throwIfAborted();
      const hardware = decodeIdentityReport(await read(REPORT.IDENTITY));
      if (this.reconnectUid && hardware.serial_number !== this.reconnectUid) {
        await s.transport.disconnect();
        if (generation === this.generation) {
          this.transition(s.transport.autoReconnect ? PHASE.RECONNECTING : PHASE.DISCONNECTED,
            { busy: false, error: REASON.WRONG_BOARD });
        }
        return;
      }
      const settings = decodeConfigReport(await read(REPORT.CONFIG), hardware);
      if (!current()) throw new BoardError(REASON.CONNECTION_CANCELLED);
      this.reconnectUid = hardware.serial_number;
      const recoveryError = s.commands.recoveryError;
      this.transition(PHASE.RECOVERING, { ...EMPTY, busy: true,
        outcome_uncertain: Boolean(recoveryError || commandPending(s.state)), error: recoveryError,
        hardware, settings, settings_supported: Boolean(hardware.capabilities & CAP.SETTINGS) }, "connection/opened");
      s.polling.scheduleExpiry();
      await s.recovery.run(device, signal, current);
      if (current()) {
        this.transition(PHASE.READY, { busy: false });
        await s.polling.pollReports();
        s.polling.pollWireless();
      }
    } catch (error) {
      if (generation !== this.generation) return;
      let failure = errorValue(error);
      try { await s.transport.disconnect(); }
      catch (closeError) {
        failure = { code: REASON.CLOSE_FAILED, detail: { cause: failure, close: closeError.message } };
      }
      if (generation === this.generation) this.clear(PHASE.DISCONNECTED, { error: failure });
    }
  }

  /** @brief User disconnect; keeps the reconnect identity while an operation is still pending. */
  async disconnect() {
    const s = this.session;
    if (s.state.busy) return;
    if (!commandPending(s.state)) this.reconnectUid = null;
    this.clear(PHASE.DISCONNECTED, { error: s.commands.recoveryError, pending_msc: null });
    try { await s.transport.disconnect(); } catch (error) { this.change({ error: errorValue(error) }); }
  }

  /** @brief Detach the transport listeners and fence the connection permanently. */
  dispose() {
    for (const [type, handler] of Object.entries(this.handlers)) {
      this.session.transport.removeEventListener(type, handler);
    }
    this.controller.abort();
  }
}
