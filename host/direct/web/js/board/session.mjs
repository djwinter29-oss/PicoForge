// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file Board session facade: wires the connection state machine, command queue, recovery, polling and use cases
 * around one transport and one domain store, and is the dashboard's only board API. It holds no logic of its own.
 */
import { createDashboardStore } from "../state/store.mjs";
import { createTimerScope, wait } from "../timers.mjs";
import { Commands, defaultPendingStore } from "./commands.mjs";
import { Connection, EMPTY, PHASE } from "./connection.mjs";
import { Operations } from "./operations.mjs";
import { Polling } from "./polling.mjs";
import { Recovery } from "./recovery.mjs";

export class BoardSession {
  /**
   * @param {import("../transport/transport.mjs").Transport} transport Byte pipe to one board.
   * @param {{store?: ReturnType<typeof createDashboardStore>, now?: () => number, sleep?: Function, timeout?: number,
   *   pendingStore?: Storage | null, timerScope?: ReturnType<typeof createTimerScope>, wirelessPolling?: boolean}} [options]
   *   `store` (shared domain store, reset here), `now`, `sleep`, `timeout` (ms per report),
   *   `pendingStore` (journal storage; default sessionStorage), `timerScope`, `wirelessPolling`.
   */
  constructor(transport, { now = () => performance.now(), sleep = wait, timeout = 3000, pendingStore, timerScope,
    wirelessPolling = true, store } = {}) {
    this.transport = transport;
    this.now = now;
    this.timeout = timeout;
    this.timers = timerScope?.createChild() ?? createTimerScope();
    this.sleep = sleep === wait ? (milliseconds, signal) => wait(milliseconds, signal, this.timers) : sleep;
    this.disposed = false;
    this.commands = new Commands(this, pendingStore === undefined ? defaultPendingStore() : pendingStore);
    this.polling = new Polling(this, wirelessPolling);
    this.recovery = new Recovery(this);
    this.operations = new Operations(this);
    const { recoveryError, restored } = this.commands;
    const initial = { ...EMPTY, link: transport.kind, phase: PHASE.DISCONNECTED, connected: false,
      reconnecting: false, busy: false, wireless_busy: false, pending_request: restored.request,
      pending_wireless: restored.wireless, pending_msc: null,
      outcome_uncertain: Boolean(recoveryError || restored.request || restored.wireless),
      error: recoveryError ?? (transport.available ? null : transport.unavailableReason) };
    this.store = store ?? createDashboardStore(initial);
    if (store) store.dispatch({ type: "replace", state: initial });
    this.connection = new Connection(this);
    this.connection.reconnectUid = restored.uid;
  }

  get state() { return this.store.getState(); }
  get device() { return this.transport.device; }
  get connectionSignal() { return this.connection.signal; }

  /**
   * @brief Commit one named domain event (state/reducer.mjs); board modules are the only callers.
   * @param {string} type
   * @param {Partial<import("../state/reducer.mjs").DashboardState>} changes
   */
  emit(type, changes) {
    if (!this.disposed) this.store.dispatch({ type, changes });
  }

  /** @brief Detach every listener and timer so a replaced session cannot write state or touch the device. */
  dispose() {
    this.disposed = true;
    this.connection.dispose();
    this.polling.receipts.cancel();
    this.timers.dispose();
    void this.transport.dispose().catch(error => console.error("Could not close the board transport", error));
  }

  connect(options) { return this.connection.connect(options); }
  disconnect() { return this.connection.disconnect(); }
  /** @param {number} opcode @param {import("./commands.mjs").CommandOptions} [options] */
  command(opcode, options) { return this.commands.command(opcode, options); }
  expireReports(suspended) { this.polling.expireReports(suspended); }
  readWireless(signal) { return this.polling.readWireless(signal); }
  pollWireless() { this.polling.pollWireless(); }
  setLed(on) { return this.operations.setLed(on); }
  reboot(confirmed) { return this.operations.reboot(confirmed); }
  setMsc(enable, force) { return this.operations.setMsc(enable, force); }
  setSelfTest(action, sizeMiB) { return this.operations.setSelfTest(action, sizeMiB); }
  saveSettings(settings, force, tcpCliEnabled) { return this.operations.saveSettings(settings, force, tcpCliEnabled); }
  /** @param {import("./operations.mjs").ConfigurationIntent} intent */
  saveConfiguration(intent) { return this.operations.saveConfiguration(intent); }
  scanWireless(signal) { return this.operations.scanWireless(signal); }
  /**
   * @param {import("./operations.mjs").WirelessPair} settings
   * @param {(progress: import("../state/reducer.mjs").ReasonValue) => void} [onProgress]
   * @param {AbortSignal} [signal]
   */
  testWireless(settings, onProgress, signal) { return this.operations.testWireless(settings, onProgress, signal); }
  cancelWireless(signal) { return this.operations.cancelWireless(signal); }
  /**
   * @param {import("./operations.mjs").WirelessConfiguration & {board: import("./operations.mjs").BootSettings}} settings
   * @param {AbortSignal} [signal]
   */
  saveWireless(settings, signal) { return this.operations.saveWireless(settings, signal); }
  authorizeConsole(nonce, uid) { return this.operations.authorizeConsole(nonce, uid); }
}
