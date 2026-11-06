/**
 * @file Transport-independent board session: one command queue, report freshness and same-board recovery.
 * The session is the only writer of the domain store; workflows use its public primitives, never its fields.
 */
import {
  buildControlRequest, decodeConfigReport, decodeControlResult, decodeIdentityReport, decodeSelfTestReport,
  decodeSettings, decodeStorageReport, decodeTelemetryReport, decodeWireless, encodeSettings, hexToBytes,
  requireValid, validSettings,
} from "../protocol/codec.mjs";
import { linkRestriction, requiredCapability } from "../protocol/commands.mjs";
import { CAP, OP, REPORT, RESULT, SAVE_SETTINGS_FORCE_MSC } from "../protocol/constants.mjs";
import { defaultPendingStore, loadPendingOperation, savePendingOperation } from "./journal.mjs";
import { ReportReceipts, TRACKED_REPORTS } from "./reports.mjs";
import { lifecycleOutcome, showsSavedSsid, wirelessPollInterval } from "./wireless-status.mjs";
import { canRun, commandBlocked, commandPending, deriveOperationEligibility } from "../state/selectors.mjs";
import { createDashboardStore } from "../state/store.mjs";
import { createTimerScope, wait, withTimeout } from "../timers.mjs";

/** @brief Per-connection facts reset on connect and disconnect; pending request facts survive. */
const EMPTY = {
  hardware: null, settings: null, settings_supported: false,
  runtime: null, memory: null, filesystem_format: null, msc: null, self_test: null,
  wireless: null, wireless_error: null, wireless_poll_error: null, recovery_message: null, recovered_opcode: null,
  freshness: Object.fromEntries(TRACKED_REPORTS.map(id => [id, "unavailable"])),
  pending_self_test: null, reboot_pending: false,
};
const sameSettings = (a, b) => a.system_clock_mhz === b.system_clock_mhz && a.sdcard_profile === b.sdcard_profile
  && (a.radios ?? 0) === (b.radios ?? 0) && a.tcp_cli_enabled === b.tcp_cli_enabled;
const commandError = (opcode, result, detail = true) => new Error(`Command ${opcode}: status ${result.status}, `
  + `error ${result.error_reason}${detail ? `, detail ${result.detail}` : ""}`);

export class BoardSession {
  /** @param {import("../transport/transport.mjs").Transport} transport */
  constructor(transport, onChange = () => {}, { now = () => performance.now(), sleep = wait, timeout = 3000,
    pendingStore, timerScope, wirelessPolling = true, store } = {}) {
    if (pendingStore === undefined) pendingStore = defaultPendingStore();
    const timers = timerScope?.createChild() ?? createTimerScope();
    Object.assign(this, { transport, onChange, now, sleep, timeout, reconnectUid: null, transaction: 0,
      wirelessOperation: null, forcedSave: null, savedSsidHex: null, generation: 0, pendingStore,
      recoveryError: null, timers, connectionController: new AbortController(), queue: null, disposed: false,
      pollTimer: null, wirelessPollTimer: null, wirelessLastRead: 0, wirelessReading: false, wirelessPolling });
    this.receipts = new ReportReceipts(timers, () => this.now(), () => this.state.wireless);
    if (sleep === wait) this.sleep = (milliseconds, signal) => wait(milliseconds, signal, timers);
    let pendingRequest = null;
    let pendingWireless = null;
    try {
      const record = loadPendingOperation(pendingStore);
      if (record) {
        this.transaction = record.request?.transaction_id ?? 0;
        this.reconnectUid = record.uid;
        if (record.kind === "control") pendingRequest = record.request;
        if (record.kind === "wireless-lifecycle") {
          pendingWireless = { operation: record.operation, generation: record.generation };
        }
      }
    } catch (error) {
      this.recoveryError = `Pending operation recovery data is unavailable or invalid: ${error.message}`;
    }
    const initial = { ...EMPTY, link: transport.kind, connected: false, reconnecting: false, busy: false,
      wireless_busy: false, pending_request: pendingRequest, pending_wireless: pendingWireless, pending_msc: null,
      outcome_uncertain: Boolean(this.recoveryError || pendingRequest || pendingWireless),
      error: this.recoveryError ?? (transport.available ? null : transport.unavailableMessage) };
    this.store = store ?? createDashboardStore(initial);
    if (store) store.dispatch({ type: "replace", state: initial });
    this.unsubscribe = this.store.subscribe(state => this.onChange(state));
    this.onInputReport = event => {
      const state = this.state;
      if (!state.connected || this.connectionSignal.aborted) return;
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
        this.update({ ...changes, freshness: { ...state.freshness, [id]: "fresh" } });
      } catch (error) { this.update({ error: error.message }); }
    };
    this.onDisconnect = () => {
      this.clearConnection();
      this.update({ reconnecting: Boolean(transport.autoReconnect && this.reconnectUid),
        error: this.recoveryError ?? (this.state.pending_request
          ? "Command outcome uncertain after disconnect; no request will be replayed."
          : this.state.pending_wireless
            ? "Wireless command outcome uncertain; reconnect to inspect its result." : null) });
    };
    this.onConnect = () => {
      if (this.reconnectUid && !this.device && !this.state.busy) void this.reconnect();
    };
    this.onSuspend = () => this.expireReports(true);
    transport.addEventListener("report", this.onInputReport);
    transport.addEventListener("disconnect", this.onDisconnect);
    transport.addEventListener("available", this.onConnect);
    transport.addEventListener("suspend", this.onSuspend);
  }

  /** @brief Detach every listener and timer so a replaced client cannot write state or touch the device. */
  dispose() {
    this.disposed = true;
    this.transport.removeEventListener("report", this.onInputReport);
    this.transport.removeEventListener("disconnect", this.onDisconnect);
    this.transport.removeEventListener("available", this.onConnect);
    this.transport.removeEventListener("suspend", this.onSuspend);
    this.receipts.cancel();
    this.connectionController.abort();
    this.timers.dispose();
    this.unsubscribe();
    this.onChange = () => {};
    void this.transport.dispose().catch(error => console.error("Could not close the board transport", error));
  }

  get state() { return this.store.getState(); }
  get device() { return this.transport.device; }
  get connectionSignal() { return this.connectionController.signal; }
  /** @brief Outstanding control request (read-only view of `state.pending_request`). */
  get outstanding() { return this.state.pending_request; }
  /** @brief Accepted scan/test lifecycle (read-only view of `state.pending_wireless`). */
  get pendingWirelessCommand() { return this.state.pending_wireless; }

  /** @brief Internal domain-store writer; the UI never calls it (architecture.test.mjs). */
  update(changes) {
    if (!this.disposed) this.store.dispatch({ type: "patch", changes });
  }

  /** @brief Persist the recovery record for the given pending facts before they enter state. */
  persist(request, wireless) {
    const uid = this.state.hardware?.serial_number ?? this.reconnectUid;
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
    this.update({ pending_request: null, pending_wireless: wireless,
      ...(!succeeded && request.opcode === OP.MSC ? { pending_msc: null } : {}) });
  }

  /** @brief Clear the recovery record after a wireless lifecycle outcome is known. */
  settleWireless() {
    this.persist(null, null);
    this.update({ pending_wireless: null });
  }

  /** @brief Operation readiness after expiring stale reports; see state/selectors.mjs. */
  eligibility() {
    this.expireReports();
    return deriveOperationEligibility(this.state);
  }

  /** @brief Throw the first failing precondition of a user operation; the UI uses the same `canRun` rule. */
  require(operation) {
    this.expireReports();
    const { reason } = canRun(this.state, operation);
    if (reason) throw new Error(reason);
  }

  expireReports(suspended = false) {
    const changes = this.receipts.expire(this.state.freshness, suspended);
    if (changes) this.update(changes);
    this.scheduleExpiry();
  }

  scheduleExpiry() {
    if (this.device) this.receipts.schedule(() => this.expireReports());
    else this.receipts.cancel();
  }

  async readReport(id, signal = this.connectionSignal, timeout = this.timeout) {
    const device = this.device;
    const report = await withTimeout(this.transport.readReport(id, signal), timeout, signal, this.timers);
    signal.throwIfAborted();
    if (device !== this.device) throw new Error("Board connection changed");
    return report;
  }

  async pollReports() {
    this.timers.clearTimeout(this.pollTimer);
    this.pollTimer = null;
    if (!this.transport.pollInterval || !this.state.connected || this.disposed) return;
    const signal = this.connectionSignal;
    const idle = () => !this.state.busy && !this.state.pending_request;
    if (idle()) {
      try {
        for (const id of TRACKED_REPORTS) {
          if (id === REPORT.SELF_TEST && !(this.state.hardware.capabilities & CAP.SELF_TEST)) continue;
          if (!idle()) break;
          const data = await this.readReport(id, signal);
          this.onInputReport({ reportId: id, data });
        }
      } catch (error) {
        if (!signal.aborted) this.update({ error: `Board status read failed: ${error.message}` });
      }
    }
    if (!signal.aborted && this.state.connected && !this.disposed) {
      this.pollTimer = this.timers.setTimeout(() => { void this.pollReports(); }, this.transport.pollInterval);
      this.pollTimer.unref?.();
    }
  }

  /** @brief The one Feature 12 poll owner: shared by both transports, self-rescheduling, independent of the UI. */
  pollWireless() {
    this.timers.clearTimeout(this.wirelessPollTimer);
    this.wirelessPollTimer = null;
    if (!this.wirelessPolling || this.disposed || !this.state.connected
      || !(this.state.hardware?.capabilities & CAP.WIRELESS)) return;
    if (this.wirelessReading) return;
    const delay = this.wirelessLastRead + wirelessPollInterval(this.state.wireless) - this.now();
    if (delay <= 0 && !this.state.busy && !this.state.reboot_pending && !this.state.pending_request
      && !this.wirelessOperation) {
      void this.readWirelessNow();
      return;
    }
    this.wirelessPollTimer = this.timers.setTimeout(() => this.pollWireless(), Math.max(delay, 250));
    this.wirelessPollTimer.unref?.();
  }

  async readWirelessNow() {
    const signal = this.connectionSignal;
    this.wirelessReading = true;
    this.wirelessLastRead = this.now();
    try { await this.readWireless(); }
    catch (error) {
      if (!signal.aborted) {
        this.expireReports();
        this.update({ wireless_poll_error: error.message });
      }
    } finally {
      this.wirelessReading = false;
      if (!this.disposed) this.pollWireless();
    }
  }

  /** @brief Read and record one Feature 12 snapshot; throws if the connection changed meanwhile. */
  async readWireless(operationSignal, device = this.device) {
    const signal = operationSignal ? AbortSignal.any([this.connectionSignal, operationSignal]) : this.connectionSignal;
    const wireless = decodeWireless(await this.readReport(REPORT.WIRELESS, signal, 1500), this.state.settings);
    if (signal.aborted || device !== this.device) throw new Error("Wireless connection changed");
    if (showsSavedSsid(wireless)) this.savedSsidHex = wireless.ssid_hex;
    this.receipts.record(REPORT.WIRELESS);
    this.update({ wireless, wireless_error: null, wireless_poll_error: null,
      freshness: { ...this.state.freshness, [REPORT.WIRELESS]: "fresh" } });
    this.scheduleExpiry();
    return wireless;
  }

  /** @brief Poll Feature 12 until the pending scan/test lifecycle completes or fails; never replays it. */
  async waitLifecycle(pending, onProgress, operationSignal) {
    const device = this.device;
    const signal = operationSignal ? AbortSignal.any([this.connectionSignal, operationSignal]) : this.connectionSignal;
    const deadline = this.now() + (pending.operation === OP.TEST_WIFI ? 35000 : 15000);
    while (this.now() < deadline) {
      if (signal.aborted) throw new Error("Wireless command outcome uncertain after disconnect");
      const status = await this.readWireless(signal, device);
      const outcome = lifecycleOutcome(status, pending);
      if (outcome) return outcome;
      onProgress?.(status.phase || "Connecting");
      await this.sleep(500, signal);
    }
    throw new Error("Wireless operation timed out; reconnect to inspect it, never replay");
  }

  /** @brief Wait for a lifecycle outcome and settle its record; an unknown outcome stays pending and uncertain. */
  async finishLifecycle(pending, onProgress, signal) {
    try {
      const outcome = await this.waitLifecycle(pending, onProgress, signal);
      this.settleWireless();
      return outcome;
    } catch (error) {
      if (this.state.pending_wireless) this.update({ outcome_uncertain: true, error: error.message });
      throw error;
    }
  }

  /**
   * @brief Run one wireless workflow exclusively. The action receives a context whose `command` carries the
   * workflow token, so only this workflow may use the queue while `wireless_busy` is set.
   */
  async runWirelessWorkflow(action) {
    this.require("wireless");
    const token = Symbol("wireless operation");
    this.wirelessOperation = token;
    this.update({ wireless_busy: true });
    const context = {
      command: (opcode, options) => this.command(opcode, { ...options, workflow: token }),
      read: signal => this.readWireless(signal),
      finishLifecycle: (pending, onProgress, signal) => this.finishLifecycle(pending, onProgress, signal),
      invalidateWireless: message => this.update({ wireless: null, wireless_error: message }),
      savedSsidHex: () => this.savedSsidHex,
      settings: () => this.state.settings,
    };
    try { return await action(context); }
    finally {
      this.wirelessOperation = null;
      this.update({ wireless_busy: false });
    }
  }

  clearConnection() {
    this.generation++;
    this.connectionController.abort(new DOMException("Device disconnected", "AbortError"));
    this.connectionController = new AbortController();
    this.timers.clearTimeout(this.pollTimer);
    this.timers.clearTimeout(this.wirelessPollTimer);
    this.pollTimer = null;
    this.wirelessPollTimer = null;
    this.wirelessLastRead = 0;
    this.savedSsidHex = null;
    this.receipts.clear();
    this.update({ ...EMPTY, connected: false, busy: false,
      outcome_uncertain: Boolean(this.recoveryError || commandPending(this.state)) });
  }

  async connect(options) {
    if (this.state.busy || this.device || this.state.reconnecting) return;
    const signal = this.connectionSignal;
    this.update({ busy: true, error: this.recoveryError });
    try {
      if (await this.transport.connect(options, signal)) await this.initializeConnection();
    } catch (error) { if (!signal.aborted) this.update({ error: error.message }); }
    finally { if (!signal.aborted) this.update({ busy: false }); }
  }

  async reconnect() {
    const signal = this.connectionSignal;
    this.update({ busy: true });
    try {
      if (await this.transport.reconnect(signal)) await this.initializeConnection();
    } catch (error) { if (!signal.aborted) this.update({ error: error.message }); }
    finally { if (!signal.aborted) this.update({ busy: false }); }
  }

  async initializeConnection() {
    const device = this.device;
    const generation = this.generation;
    const signal = this.connectionController.signal;
    const current = () => generation === this.generation && device === this.device;
    this.update({ busy: true });
    try {
      signal.throwIfAborted();
      const hardware = decodeIdentityReport(await withTimeout(this.transport.readReport(REPORT.IDENTITY, signal),
        this.timeout, signal, this.timers));
      const expectedUid = this.reconnectUid;
      if (expectedUid && hardware.serial_number !== expectedUid) {
        await this.transport.disconnect();
        if (generation === this.generation) this.update({ reconnecting: this.transport.autoReconnect, busy: false,
          error: "Select the original board to resolve its pending operation." });
        return;
      }
      const settings = decodeConfigReport(await withTimeout(this.transport.readReport(REPORT.CONFIG, signal),
        this.timeout, signal, this.timers), hardware);
      if (!current()) throw new Error("Connection cancelled");
      this.reconnectUid = hardware.serial_number;
      this.update({ ...EMPTY, connected: true, reconnecting: false, busy: true,
        outcome_uncertain: Boolean(this.recoveryError || commandPending(this.state)), error: this.recoveryError,
        hardware, settings, settings_supported: Boolean(hardware.capabilities & CAP.SETTINGS) });
      this.scheduleExpiry();
      const request = this.state.pending_request;
      if (request) {
        try {
          const result = await this.pollResult(device, request, signal);
          const succeeded = result.status === RESULT.COMPLETED;
          if (succeeded && request.settings && (!sameSettings(result.settings, request.settings)
            || result.commit_mask !== request.commit_mask)) {
            throw new Error("Firmware did not confirm the selected values after reconnect");
          }
          this.retire(request, succeeded);
          this.update({ outcome_uncertain: false, error: succeeded ? null : commandError(result.opcode, result, false)
            .message, recovered_opcode: succeeded ? result.opcode : null, recovery_message: !succeeded ? null
            : result.opcode === OP.CANCEL_TEST ? "Wireless test cancellation confirmed after reconnect."
              : "Pending command result confirmed after reconnect." });
          this.forcedSave?.settle({ result });
        } catch (error) {
          this.update({ outcome_uncertain: true, error: error.message });
          this.forcedSave?.settle({ error });
        }
      }
      const pending = this.state.pending_wireless;
      if (pending && !this.state.pending_request) {
        try {
          const outcome = await this.waitLifecycle(pending);
          if (!current()) throw new Error("Wireless command outcome uncertain after disconnect");
          this.settleWireless();
          this.update({ outcome_uncertain: Boolean(this.recoveryError),
            error: this.recoveryError ?? (outcome.kind === "failed" ? outcome.message : null),
            recovery_message: outcome.kind === "completed" ? "Wireless operation confirmed after reconnect." : null });
        } catch (error) {
          this.update({ outcome_uncertain: true, error: error.message });
        }
      }
      if (current()) {
        this.update({ busy: false });
        await this.pollReports();
        this.pollWireless();
      }
    } catch (error) {
      if (generation !== this.generation) return;
      try { await this.transport.disconnect(); }
      catch (closeError) { error = new Error(`${error.message}; closing the transport failed: ${closeError.message}`); }
      if (generation === this.generation) {
        this.clearConnection();
        this.update({ reconnecting: false, error: error.message });
      }
    }
  }

  async disconnect() {
    if (this.state.busy) return;
    if (!commandPending(this.state)) this.reconnectUid = null;
    this.clearConnection();
    this.update({ reconnecting: false, error: this.recoveryError, pending_msc: null });
    try { await this.transport.disconnect(); } catch (error) { this.update({ error: error.message }); }
  }

  async pollResult(device, request, signal = this.connectionController.signal) {
    const deadline = this.now() + this.timeout;
    const generation = this.generation;
    for (let attempt = 0; attempt < Math.ceil(this.timeout / 50) && this.now() < deadline; attempt++) {
      if (signal.aborted || device !== this.device) throw new Error("Command outcome uncertain after disconnect");
      const report = await withTimeout(this.transport.readReport(REPORT.CONTROL, signal), deadline - this.now(),
        signal, this.timers);
      if (signal.aborted || device !== this.device || generation !== this.generation) {
        throw new Error("Command outcome uncertain after disconnect");
      }
      const result = decodeControlResult(report, { ble: this.state.link === "ble" });
      if (result.transaction_id === request.transaction_id && result.opcode === request.opcode
        && result.status > RESULT.PENDING) {
        if (result.status === RESULT.COMPLETED && request.opcode === OP.SCAN_ENTRY
          && ((result.data[0] | result.data[1] << 8) !== request.scan_generation
            || result.data[2] !== request.scan_index)) {
          throw new Error("Scan entry did not match the requested generation and index");
        }
        return result;
      }
      await this.sleep(50, signal);
    }
    throw new Error("Command outcome uncertain: result timeout; disconnect/reconnect to read it, never replay");
  }

  /** @brief Check a console challenge against this board, then return its link-specific 16-byte proof. */
  async authorizeConsole(nonce, uid) {
    if (!(nonce instanceof Uint8Array) || nonce.length !== 16
      || !(uid instanceof Uint8Array) || uid.length !== 8) {
      throw new Error("Invalid TCP CLI authentication challenge");
    }
    if (!this.state.connected || !this.state.hardware) {
      throw new Error("Connect the matching PicoForge board before authorizing its Wi-Fi console");
    }
    const serial = this.state.hardware.serial_number;
    if (serial) {
      const expectedUid = hexToBytes(serial);
      if (uid.some((byte, index) => byte !== expectedUid[index])) {
        throw new Error("The Wi-Fi console challenge belongs to a different board");
      }
    }
    return this.transport.authorizeConsole
      ? this.transport.authorizeConsole(nonce, uid, this.connectionSignal) : this.tcpCliAuthProof(nonce);
  }

  async tcpCliAuthProof(nonce) {
    if (!this.device || !this.state.connected || !(this.state.hardware?.capabilities & CAP.WIRELESS)) {
      throw new Error("Connect the matching PicoForge board over USB to authorize its Wi-Fi console");
    }
    const result = await this.command(OP.TCP_CLI_PROOF, { args: nonce });
    return result.data.slice(0, 16);
  }

  /**
   * @brief Send one control request and wait for its authoritative result; the outcome stays persisted until known.
   * `options.reboots` marks the board as rebooting after success; other follow-up state belongs to the caller.
   */
  async command(opcode, options) {
    const restriction = linkRestriction(this.state.link, opcode, options?.flags ?? 0);
    if (restriction) throw new Error(restriction);
    if (this.queue) throw new Error("Command unavailable while another command owns the queue");
    const args = options?.args ? [...options.args] : undefined;
    const signal = options?.signal
      ? AbortSignal.any([this.connectionSignal, options.signal]) : this.connectionSignal;
    this.queue = { opcode, signal };
    try {
      const result = await this.executeCommand(opcode, { ...options, args, signal });
      if (options?.reboots) this.update({ reboot_pending: true });
      return result;
    } finally {
      args?.fill(0);
      this.queue = null;
    }
  }

  async executeCommand(opcode, options) {
    this.expireReports();
    const signal = options.signal;
    if (signal.aborted) throw signal.reason ?? new DOMException("Operation cancelled", "AbortError");
    if (!this.device || commandBlocked(this.state)
      || this.state.wireless_busy && options.workflow !== this.wirelessOperation) {
      throw new Error("Command unavailable or previous outcome uncertain");
    }
    const bit = requiredCapability(opcode);
    if (bit && !(this.state.hardware.capabilities & bit)) throw new Error("Unavailable capability");
    const transaction = this.transaction % 255 + 1;
    const report = buildControlRequest(transaction, opcode, options);
    // A forced save may reset USB; connection initialization settles the result after same-board reconnect.
    const forced = opcode === OP.SAVE_SETTINGS && report[3] & SAVE_SETTINGS_FORCE_MSC ? {} : null;
    if (forced) forced.done = new Promise(resolve => { forced.settle = outcome => resolve(forced.outcome = outcome); });
    this.forcedSave = forced;
    this.transaction = transaction;
    const request = { transaction_id: transaction, opcode };
    if (opcode === OP.SCAN_ENTRY) {
      request.scan_index = report[4];
      request.scan_generation = report[5] | report[6] << 8;
    }
    if (options.lifecycle) request.lifecycle = options.lifecycle;
    if (opcode === OP.SAVE_SETTINGS) {
      request.settings = decodeSettings(report.subarray(4, 8));
      request.commit_mask = options.commitMask ?? 0;
    }
    const device = this.device;
    const generation = this.generation;
    this.persist(request, null);
    this.update({ busy: true, pending_request: request, error: null, recovery_message: null, recovered_opcode: null,
      ...(opcode === OP.MSC ? { pending_msc: report[4] === 1 } : {}) });
    let sent = false;
    try {
      await withTimeout(this.transport.writeReport(REPORT.CONTROL, report, signal), this.timeout, signal, this.timers);
      sent = true;
      if (generation !== this.generation) throw new Error("Command outcome uncertain after disconnect");
      const result = await this.pollResult(device, request, signal);
      if (result.status !== RESULT.COMPLETED) {
        this.retire(request, false);
        throw commandError(opcode, result);
      }
      if (request.settings) {
        if (!sameSettings(result.settings, request.settings) || result.commit_mask !== request.commit_mask) {
          this.update({ outcome_uncertain: true });
          throw new Error("Firmware did not confirm the selected values");
        }
        this.update({ settings: result.settings });
      }
      this.retire(request, true);
      return result;
    } catch (error) {
      if (forced && sent && this.reconnectUid && !(generation === this.generation && this.state.outcome_uncertain)
        && (this.state.pending_request || forced.outcome)) {
        const { result } = forced.outcome ?? await withTimeout(forced.done, 5000, undefined, this.timers)
          .catch(() => ({}));
        if (result?.transaction_id === transaction && result.opcode === opcode) {
          if (result.status !== RESULT.COMPLETED) throw commandError(opcode, result, false);
          if (sameSettings(result.settings, request.settings) && result.commit_mask === request.commit_mask) {
            this.update({ settings: result.settings, outcome_uncertain: false });
            return result;
          }
        }
        if (this.state.error) throw new Error(this.state.error);
      }
      // SET rejection is authoritative: do not poll or accept a prior result.
      if (!sent && !["TimeoutError", "AbortError"].includes(error.name) && generation === this.generation) {
        try { this.retire(request, false); }
        catch (storageError) {
          this.update({
            error: `Command was not sent, but recovery state could not be cleared: ${storageError.message}` });
        }
      }
      if (generation === this.generation) {
        this.update({ outcome_uncertain: commandPending(this.state), error: error.message });
      }
      throw error;
    } finally {
      this.forcedSave = null;
      report.fill(0);
      if (generation === this.generation) this.update({ busy: false });
    }
  }

  async setLed(on) {
    requireValid(typeof on === "boolean", "LED state");
    this.require("led");
    return this.command(OP.LED, { args: [Number(on)] });
  }

  async reboot(confirmed = false) {
    if (confirmed !== true) throw new Error("Reboot requires explicit confirmation");
    this.require("reboot");
    return this.command(OP.REBOOT, { flags: 1, reboots: true });
  }

  async setMsc(enable, force = false) {
    requireValid(typeof enable === "boolean" && typeof force === "boolean" && !(force && enable), "MSC action");
    this.require("msc");
    if (force && !this.state.msc.enabled) throw new Error("Fresh idle storage status required");
    return this.command(OP.MSC, { flags: Number(force), args: [force ? 2 : Number(enable)] });
  }

  async setSelfTest(action, sizeMiB = 32) {
    requireValid(["start", "cancel"].includes(action), "self-test action");
    this.require("selfTest");
    if (action === "start" && (this.state.self_test.active || !this.state.self_test.volume_mounted
      || !this.state.memory?.initialized)) throw new Error("Fresh self-test and firmware-owned storage required");
    if (action === "cancel") sizeMiB = 0;
    requireValid(Number.isInteger(sizeMiB) && sizeMiB >= (action === "start" ? 1 : 0) && sizeMiB <= 1024,
      "self-test size");
    const size = new Uint8Array(4);
    new DataView(size.buffer).setUint32(0, sizeMiB, true);
    const result = await this.command(OP.SELF_TEST, { args: [Number(action === "start"), ...size] });
    this.update({ pending_self_test: { sequence: this.state.self_test?.sequence ?? null } });
    return result;
  }

  /** @brief Save next-boot settings; throws on refusal or failure, and marks the board as rebooting on success. */
  async saveSettings(settings, force = false, tcpCliEnabled) {
    requireValid(validSettings(settings), "boot settings");
    requireValid(typeof force === "boolean", "forced save confirmation");
    requireValid(tcpCliEnabled === undefined || typeof tcpCliEnabled === "boolean", "TCP CLI setting");
    this.require("settings");
    if (this.state.msc.enabled !== force) throw new Error("Fresh idle firmware-owned storage required before saving.");
    const tcp = tcpCliEnabled ?? settings.tcp_cli_enabled ?? this.state.settings?.tcp_cli_enabled ?? false;
    return this.command(OP.SAVE_SETTINGS, { flags: Number(force), args: encodeSettings(settings, tcp), reboots: true });
  }
}
