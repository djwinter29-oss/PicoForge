/**
 * @file Report receipts and freshness expiry, independent of transport and UI.
 */
import { REPORT, REPORT_LIFETIME_MS } from "../protocol/constants.mjs";
import { wirelessPollInterval } from "./wireless-status.mjs";

/** @brief Report IDs whose snapshot expires; Feature 12 lifetime follows its poll interval. */
export const TRACKED_REPORTS = [REPORT.TELEMETRY, REPORT.STORAGE, REPORT.SELF_TEST];

/** @brief State cleared when a report expires. */
const STALE = {
  [REPORT.TELEMETRY]: { runtime: null },
  [REPORT.STORAGE]: { memory: null, filesystem_format: null, msc: null },
  [REPORT.SELF_TEST]: { self_test: null },
  [REPORT.WIRELESS]: { wireless: null, wireless_error: "Wireless status is stale" },
};

/** @brief Lifetime of one report snapshot in milliseconds. */
export const reportLifetime = (id, wireless) => id === REPORT.WIRELESS
  ? 3 * wirelessPollInterval(wireless) : REPORT_LIFETIME_MS[id];

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
