// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file Report polling and freshness: input reports, the status-report poll, the one Feature 12 poll owner and
 * scan/test lifecycle observation. Writes only `report/*` domain events.
 */
import { decodeSelfTestReport, decodeStorageReport, decodeTelemetryReport, decodeWireless }
  from "../protocol/codec.mjs";
import {
  CAP, OP, REPORT, REPORT_LIFETIME_MS, SCAN_STATE, STAGED_EXPIRED, WIFI_STATE,
} from "../protocol/constants.mjs";
import { OPCODES } from "../protocol/opcodes.mjs";
import { BoardError, errorValue, REASON } from "../state/errors.mjs";
import { withTimeout } from "../timers.mjs";

/** @brief Report IDs whose snapshot expires; Feature 12 lifetime follows its poll interval. */
export const TRACKED_REPORTS = [REPORT.TELEMETRY, REPORT.STORAGE, REPORT.SELF_TEST];

/** @brief State cleared when a report expires. */
const STALE = {
  [REPORT.TELEMETRY]: { runtime: null },
  [REPORT.STORAGE]: { memory: null, filesystem_format: null, msc: null },
  [REPORT.SELF_TEST]: { self_test: null },
  [REPORT.WIRELESS]: { wireless: null, wireless_error: REASON.WIRELESS_STALE },
};

/** @brief Feature 12 poll interval: fast while a Wi-Fi test or scan runs, otherwise 2 s. */
export const wirelessPollInterval = wireless => wireless?.state === WIFI_STATE.TESTING
  || wireless?.scan_state === SCAN_STATE.SCANNING ? 500 : 2000;

/** @brief Whether a snapshot shows the saved (not staged or under test) SSID. */
const showsSavedSsid = wireless => !wireless.staged_mask
  && ![WIFI_STATE.TESTING, WIFI_STATE.TEST_JOINED, WIFI_STATE.TEST_FAILED].includes(wireless.state);

/** @brief Lifetime of one report snapshot in milliseconds. */
export const reportLifetime = (id, wireless) => id === REPORT.WIRELESS
  ? 3 * wirelessPollInterval(wireless) : REPORT_LIFETIME_MS[id];

/**
 * @brief Interpret one Feature 12 snapshot for an accepted scan/test lifecycle.
 * @param status Decoded Feature 12 snapshot.
 * @param pending Lifecycle record `{operation, generation}`.
 * @return `{kind: "completed", result}`, `{kind: "failed", error: BoardError}`, or null while still running; throws
 *   when the board started a different generation.
 */
export function lifecycleOutcome(status, pending) {
  if (pending.operation === OP.SCAN) {
    if (status.scan_generation !== pending.generation) throw new BoardError(REASON.SCAN_GENERATION_CHANGED);
    if (status.scan_state === SCAN_STATE.FAILED) {
      return { kind: "failed", error: new BoardError(REASON.WIRELESS_SCAN_FAILED) };
    }
    if (status.scan_state === SCAN_STATE.COMPLETE) return { kind: "completed", result: status };
  } else {
    if (status.test_generation !== pending.generation) throw new BoardError(REASON.TEST_GENERATION_CHANGED);
    if (status.state === WIFI_STATE.TEST_FAILED || status.staged_state & STAGED_EXPIRED) {
      return { kind: "failed", error: new BoardError(REASON.TEST_FAILED, status.detail || null) };
    }
    if (status.state === WIFI_STATE.TEST_JOINED && status.pair_tested) {
      // ponytail: no lifetime is transmitted; the board starts a 600s window on success. Subtract polling latency.
      return { kind: "completed", result: { ...status, remaining_seconds: 599 } };
    }
  }
  return null;
}

/** @brief Receipt clock for fresh reports; owns its one expiry timer. */
export class ReportReceipts {
  /**
   * @param timers Timer scope owning the expiry timer.
   * @param {() => number} now Monotonic clock in milliseconds.
   * @param {() => object | null} wireless Current wireless snapshot, for the Feature 12 lifetime.
   */
  constructor(timers, now, wireless) {
    this.timers = timers;
    this.now = now;
    this.wireless = wireless;
    /** @type {Map<number, number>} Report ID to receipt time. */
    this.received = new Map();
    this.timer = null;
  }

  get(id) { return this.received.get(id); }
  get size() { return this.received.size; }

  record(id) { this.received.set(id, this.now()); }

  /**
   * @brief Drop expired (or, when suspended, all) receipts.
   * @return State changes with `freshness`, or null when nothing expired.
   */
  expire(freshness, suspended = false) {
    const changes = {};
    const next = { ...freshness };
    for (const [id, received] of this.received) {
      if (!suspended && this.now() - received < reportLifetime(id, this.wireless())) continue;
      this.received.delete(id);
      next[id] = "stale";
      Object.assign(changes, STALE[id]);
    }
    return Object.keys(changes).length ? { ...changes, freshness: next } : null;
  }

  /** @brief Arm one timer for the earliest expiry; nothing is armed while there are no receipts. */
  schedule(onExpire) {
    this.cancel();
    if (!this.received.size) return;
    const deadline = Math.min(...[...this.received].map(([id, received]) =>
      received + reportLifetime(id, this.wireless())));
    this.timer = this.timers.setTimeout(() => {
      this.timer = null;
      onExpire();
    }, Math.max(0, deadline - this.now()));
    this.timer.unref?.();
  }

  cancel() {
    this.timers.clearTimeout(this.timer);
    this.timer = null;
  }

  clear() {
    this.cancel();
    this.received.clear();
  }
}

/** @brief Report reader and freshness owner for one board session. */
export class Polling {
  /**
   * @param {import("./session.mjs").BoardSession} session Facade giving access to the transport, store and peers.
   * @param {boolean} enabled Whether the session polls Feature 12 on its own.
   */
  constructor(session, enabled) {
    this.session = session;
    /** Feature 12 polling switch; tests disable it to drive reads explicitly. */
    this.enabled = enabled;
    this.receipts = new ReportReceipts(session.timers, () => session.now(), () => session.state.wireless);
    this.pollTimer = null;
    this.wirelessPollTimer = null;
    /** Clock value of the last Feature 12 poll. */
    this.wirelessLastRead = 0;
    this.wirelessReading = false;
    /** SSID hex of the last snapshot showing the saved (not staged) network; used to detect a changed SSID. */
    this.savedSsidHex = null;
  }

  /** @brief Record one status report from the transport or the poll. */
  onInputReport(event) {
    const s = this.session;
    const state = s.state;
    if (!state.connected || s.connectionSignal.aborted) return;
    const id = event.reportId;
    if (!TRACKED_REPORTS.includes(id)
      || (id === REPORT.SELF_TEST && !(state.hardware.capabilities & CAP.SELF_TEST))) return;
    try {
      const changes = id === REPORT.TELEMETRY ? { runtime: decodeTelemetryReport(event.data, state.hardware) }
        : id === REPORT.STORAGE ? decodeStorageReport(event.data, state.hardware)
          : { self_test: decodeSelfTestReport(event.data) };
      const quiet = !state.busy && !state.pending_request;
      if (id === REPORT.STORAGE && quiet && state.pending_msc != null && (changes.msc.request_rejected
        || changes.msc.enabled === state.pending_msc && !changes.msc.transition_pending)) {
        changes.pending_msc = null;
      }
      if (id === REPORT.SELF_TEST && quiet && state.pending_self_test
        && changes.self_test.sequence !== state.pending_self_test.sequence) {
        changes.pending_self_test = null;
      }
      this.receipts.record(id);
      this.scheduleExpiry();
      s.emit("report/received", { ...changes, freshness: { ...state.freshness, [id]: "fresh" } });
    } catch (error) { s.emit("report/failed", { error: errorValue(error) }); }
  }

  expireReports(suspended = false) {
    const changes = this.receipts.expire(this.session.state.freshness, suspended);
    if (changes) this.session.emit("report/expired", changes);
    this.scheduleExpiry();
  }

  scheduleExpiry() {
    if (this.session.device) this.receipts.schedule(() => this.expireReports());
    else this.receipts.cancel();
  }

  async readReport(id, signal = this.session.connectionSignal, timeout = this.session.timeout) {
    const s = this.session;
    const device = s.device;
    const report = await withTimeout(s.transport.readReport(id, signal), timeout, signal, s.timers);
    signal.throwIfAborted();
    if (device !== s.device) throw new BoardError(REASON.CONNECTION_CHANGED);
    return report;
  }

  async pollReports() {
    const s = this.session;
    s.timers.clearTimeout(this.pollTimer);
    this.pollTimer = null;
    if (!s.transport.pollInterval || !s.state.connected || s.disposed) return;
    const signal = s.connectionSignal;
    const idle = () => !s.state.busy && !s.state.pending_request;
    if (idle()) {
      try {
        for (const id of TRACKED_REPORTS) {
          if (id === REPORT.SELF_TEST && !(s.state.hardware.capabilities & CAP.SELF_TEST)) continue;
          if (!idle()) break;
          const data = await this.readReport(id, signal);
          this.onInputReport({ reportId: id, data });
        }
      } catch (error) {
        if (!signal.aborted) {
          s.emit("report/failed", { error: { code: REASON.STATUS_READ_FAILED, detail: errorValue(error) } });
        }
      }
    }
    if (!signal.aborted && s.state.connected && !s.disposed) {
      this.pollTimer = s.timers.setTimeout(() => { void this.pollReports(); }, s.transport.pollInterval);
      this.pollTimer.unref?.();
    }
  }

  /** @brief The one Feature 12 poll owner: shared by both transports, self-rescheduling, independent of the UI. */
  pollWireless() {
    const s = this.session;
    s.timers.clearTimeout(this.wirelessPollTimer);
    this.wirelessPollTimer = null;
    if (!this.enabled || s.disposed || !s.state.connected || !(s.state.hardware?.capabilities & CAP.WIRELESS)) return;
    if (this.wirelessReading) return;
    const delay = this.wirelessLastRead + wirelessPollInterval(s.state.wireless) - s.now();
    if (delay <= 0 && !s.state.busy && !s.state.reboot_pending && !s.state.pending_request
      && !s.state.wireless_busy) {
      void this.readWirelessNow();
      return;
    }
    this.wirelessPollTimer = s.timers.setTimeout(() => this.pollWireless(), Math.max(delay, 250));
    this.wirelessPollTimer.unref?.();
  }

  async readWirelessNow() {
    const s = this.session;
    const signal = s.connectionSignal;
    this.wirelessReading = true;
    this.wirelessLastRead = s.now();
    try { await this.readWireless(); }
    catch (error) {
      if (!signal.aborted) {
        this.expireReports();
        s.emit("report/failed", { wireless_poll_error: errorValue(error) });
      }
    } finally {
      this.wirelessReading = false;
      if (!s.disposed) this.pollWireless();
    }
  }

  /** @brief Read and record one Feature 12 snapshot; throws if the connection changed meanwhile. */
  async readWireless(operationSignal, device = this.session.device) {
    const s = this.session;
    const signal = operationSignal ? AbortSignal.any([s.connectionSignal, operationSignal]) : s.connectionSignal;
    const wireless = decodeWireless(await this.readReport(REPORT.WIRELESS, signal, 1500), s.state.settings);
    if (signal.aborted || device !== s.device) throw new BoardError(REASON.WIRELESS_CONNECTION_CHANGED);
    if (showsSavedSsid(wireless)) this.savedSsidHex = wireless.ssid_hex;
    this.receipts.record(REPORT.WIRELESS);
    s.emit("report/received", { wireless, wireless_error: null, wireless_poll_error: null,
      freshness: { ...s.state.freshness, [REPORT.WIRELESS]: "fresh" } });
    this.scheduleExpiry();
    return wireless;
  }

  /** @brief Poll Feature 12 until the pending scan/test lifecycle completes or fails; never replays it. */
  async waitLifecycle(pending, onProgress, operationSignal) {
    const s = this.session;
    const device = s.device;
    const signal = operationSignal ? AbortSignal.any([s.connectionSignal, operationSignal]) : s.connectionSignal;
    const deadline = s.now() + OPCODES[pending.operation].lifecycle.timeoutMs;
    while (s.now() < deadline) {
      if (signal.aborted) throw new BoardError(REASON.WIRELESS_UNCERTAIN);
      const status = await this.readWireless(signal, device);
      const outcome = lifecycleOutcome(status, pending);
      if (outcome) return outcome;
      onProgress?.(status.detail ? { code: REASON.WIFI_DETAIL, detail: status.detail } : REASON.WIRELESS_CONNECTING);
      await s.sleep(500, signal);
    }
    throw new BoardError(REASON.WIRELESS_TIMEOUT);
  }

  /** @brief Stop every poll and forget per-connection receipts. */
  reset() {
    const timers = this.session.timers;
    timers.clearTimeout(this.pollTimer);
    timers.clearTimeout(this.wirelessPollTimer);
    this.pollTimer = null;
    this.wirelessPollTimer = null;
    this.wirelessLastRead = 0;
    this.savedSsidHex = null;
    this.receipts.clear();
  }
}
