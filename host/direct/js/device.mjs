/**
 * WebHID client for one PicoForge board: connection, report freshness, one outstanding control transaction and
 * same-tab recovery. Pure codecs live in protocol.mjs; persisted recovery records live in recovery.mjs.
 */
import {
  CAP, DEVICE_FILTER, OP, PRODUCT_IDS, SAVE_SETTINGS_FORCE_MSC, buildControlRequest, decodeConfigReport,
  decodeControlResult, decodeIdentityReport, decodeSelfTestReport, decodeSettings, decodeStorageReport,
  decodeTelemetryReport, encodeSettings, hexToBytes, matchesPicoForge, requireValid, validSettings, wait, withTimeout,
} from "./protocol.mjs?v=48";
import { defaultPendingStore, loadPendingOperation, savePendingOperation } from "./recovery.mjs?v=48";
import { deriveOperationEligibility } from "./eligibility.mjs?v=48";
import { waitLifecycle, wirelessPollInterval } from "./wireless.mjs?v=48";

export * from "./protocol.mjs?v=48";

const EMPTY = {
  hardware: null, settings: null, settings_supported: false, settings_message: null,
  runtime: null, memory: null, filesystem_format: null, msc: null, self_test: null,
  wireless: null, wireless_error: null, recovery_message: null, recovered_opcode: null,
  freshness: { 3: "unavailable", 10: "unavailable", 11: "unavailable" },
  self_test_command_pending: false, msc_command_pending: false, reboot_pending: false,
};
const sameSettings = (a, b) => a.system_clock_mhz === b.system_clock_mhz && a.sdcard_profile === b.sdcard_profile
  && (a.radios ?? 0) === (b.radios ?? 0) && a.tcp_cli_enabled === b.tcp_cli_enabled;
/** Capability bit each opcode requires; every wireless opcode (6 and above) requires CAP.WIRELESS. */
const REQUIRED_CAP = { [OP.LED]: CAP.LED, [OP.REBOOT]: CAP.REBOOT, [OP.SELF_TEST]: CAP.SELF_TEST,
  [OP.SAVE_SETTINGS]: CAP.SETTINGS };
export class PicoForgeWebHid {
  constructor(hid, onChange = () => {}, { now = () => performance.now(), sleep = wait, timeout = 3000,
    pendingStore } = {}) {
    if (pendingStore === undefined) pendingStore = defaultPendingStore();
    Object.assign(this, { hid, onChange, now, sleep, timeout, device: null, reconnectUid: null,
      transaction: 0, outstanding: null, pendingWirelessCommand: null,
      wirelessOperation: null, forcedSave: null,
      receipts: new Map(), expiryTimer: null, generation: 0, mscPendingEnabled: null,
      selfTestPendingSequence: null, pendingStore, recoveryError: null });
    try {
      const record = loadPendingOperation(pendingStore);
      if (record) {
        this.transaction = record.request?.transaction_id ?? 0;
        this.reconnectUid = record.uid;
        if (record.kind === "control") this.outstanding = record.request;
        if (record.kind === "wireless-lifecycle") this.pendingWirelessCommand = record;
      }
    } catch (error) {
      this.recoveryError = `Pending operation recovery data is unavailable or invalid: ${error.message}`;
    }
    this.state = { ...EMPTY, link: "usb", connected: false, reconnecting: false, busy: false,
      outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
      error: this.recoveryError
        ?? (hid ? null : "WebHID is unavailable; use a supported browser on localhost or HTTPS") };
    this.onInputReport = event => {
      if (event.device !== this.device || !this.state.connected) return;
      const id = event.reportId;
      if (![3, 10, 11].includes(id) || (id === 11 && !(this.state.hardware.capabilities & CAP.SELF_TEST))) return;
      try {
        const changes = id === 3 ? { runtime: decodeTelemetryReport(event.data, this.state.hardware) }
          : id === 10 ? decodeStorageReport(event.data, this.state.hardware)
            : { self_test: decodeSelfTestReport(event.data) };
        if (id === 10 && !this.state.busy && !this.outstanding && this.mscPendingEnabled !== null
          && (changes.msc.request_rejected ||
            (changes.msc.enabled === this.mscPendingEnabled && !changes.msc.transition_pending))) {
          this.mscPendingEnabled = null;
          changes.msc_command_pending = false;
        }
        if (id === 11 && !this.state.busy && !this.outstanding
          && changes.self_test.sequence !== this.selfTestPendingSequence) {
          changes.self_test_command_pending = false;
        }
        this.receipts.set(id, this.now());
        this.scheduleExpiry();
        this.update({ ...changes, freshness: { ...this.state.freshness, [id]: "fresh" } });
      } catch (error) { this.update({ error: error.message }); }
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      this.clearConnection();
      this.update({ reconnecting: Boolean(this.reconnectUid), error: this.recoveryError ?? (this.outstanding
        ? "Command outcome uncertain after disconnect; no request will be replayed."
        : this.pendingWirelessCommand
          ? "Wireless command outcome uncertain; reconnect to inspect its result." : null) });
    };
    this.onConnect = event => {
      if (this.reconnectUid && matchesPicoForge(event.device) && !this.device) void this.attachDevice(event.device);
    };
    this.onSuspend = event => { if (event.device === this.device) this.expireReports(true); };
    hid?.addEventListener("disconnect", this.onDisconnect);
    hid?.addEventListener("connect", this.onConnect);
    hid?.addEventListener("suspend", this.onSuspend);
  }

  /** @brief Detach every listener and timer so a replaced client cannot write state or touch the device. */
  dispose() {
    this.hid?.removeEventListener("disconnect", this.onDisconnect);
    this.hid?.removeEventListener("connect", this.onConnect);
    this.hid?.removeEventListener("suspend", this.onSuspend);
    this.device?.removeEventListener("inputreport", this.onInputReport);
    clearTimeout(this.expiryTimer);
    this.onChange = () => {};
  }

  update(changes) {
    this.state = { ...this.state, ...changes };
    this.onChange(this.state);
  }

  persistPendingOperation() {
    const uid = this.state.hardware?.serial_number ?? this.reconnectUid;
    const record = this.outstanding ? { version: 2, kind: "control", uid, request: this.outstanding }
      : this.pendingWirelessCommand ? { version: 2, kind: "wireless-lifecycle", uid, ...this.pendingWirelessCommand }
        : null;
    savePendingOperation(this.pendingStore, record);
  }

  clearPersistedOperation() { savePendingOperation(this.pendingStore, null); }

  /**
   * @brief Retire the outstanding transaction. A successful lifecycle request becomes the pending wireless command;
   * a failed MSC request releases its pending ownership change.
   */
  retire(request, succeeded) {
    if (succeeded && request.lifecycle) {
      this.pendingWirelessCommand = request.lifecycle;
      this.outstanding = null;
      this.persistPendingOperation();
    } else {
      this.clearPersistedOperation();
      this.outstanding = null;
    }
    if (!succeeded && request.opcode === OP.MSC) {
      this.mscPendingEnabled = null;
      this.update({ msc_command_pending: false });
    }
  }

  /** @brief Record a decoded Feature 12 snapshot as fresh wireless state; its lifetime follows the poll interval. */
  recordWireless(wireless) {
    if (!wireless.staged_mask && ![4, 5, 6].includes(wireless.state)) this.savedSsidHex = wireless.ssid_hex;
    this.receipts.set(12, this.now());
    this.update({ wireless, wireless_error: null, freshness: { ...this.state.freshness, 12: "fresh" } });
    this.scheduleExpiry();
  }

  /** @brief Run one wireless workflow exclusively; its commands pass the token as `workflow`. */
  async exclusiveWireless(action) {
    if (!this.eligibility().wirelessReady) {
      throw new Error("Wireless configuration requires fresh idle USB storage, with sharing disabled.");
    }
    const token = Symbol("wireless operation");
    this.wirelessOperation = token;
    try { return await action(token); }
    finally { this.wirelessOperation = null; }
  }

  /** @brief Clear the recovery record after a wireless lifecycle outcome is known. */
  settleWireless() {
    this.clearPersistedOperation();
    this.pendingWirelessCommand = null;
  }

  /** @brief Single source of operation readiness; see eligibility.mjs. */
  eligibility() {
    this.expireReports();
    return deriveOperationEligibility(this.state, { outstanding: this.outstanding ?? this.pendingWirelessCommand,
      wirelessBusy: Boolean(this.wirelessOperation) });
  }

  expireReports(suspended = false) {
    const changes = {};
    const freshness = { ...this.state.freshness };
    for (const [id, received] of this.receipts) {
      if (!suspended && this.now() - received < this.reportLifetime(id)) continue;
      this.receipts.delete(id);
      freshness[id] = "stale";
      Object.assign(changes, id === 3 ? { runtime: null }
        : id === 10 ? { memory: null, filesystem_format: null, msc: null }
          : id === 12 ? { wireless: null, wireless_error: "Wireless status is stale" } : { self_test: null });
    }
    if (Object.keys(changes).length) this.update({ ...changes, freshness });
    this.scheduleExpiry();
  }

  scheduleExpiry() {
    clearTimeout(this.expiryTimer);
    this.expiryTimer = null;
    if (!this.device || !this.receipts.size) return;
    const deadline = Math.min(...[...this.receipts].map(([id, received]) =>
      received + this.reportLifetime(id)));
    this.expiryTimer = setTimeout(() => {
      this.expiryTimer = null;
      this.expireReports();
    }, Math.max(0, deadline - this.now()));
    this.expiryTimer.unref?.();
  }

  reportLifetime(id) { return id === 12 ? 3 * wirelessPollInterval(this.state.wireless) : id === 3 ? 750 : 1500; }

  clearConnection() {
    this.generation++;
    this.device?.removeEventListener("inputreport", this.onInputReport);
    clearTimeout(this.expiryTimer);
    this.expiryTimer = null;
    this.device = null;
    this.savedSsidHex = null;
    this.receipts.clear();
    this.update({ ...EMPTY, connected: false, busy: false,
      outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
      msc_command_pending: this.mscPendingEnabled !== null });
  }

  async connect() {
    if (!this.hid || this.state.busy || this.device || this.state.reconnecting) return;
    this.update({ busy: true, error: this.recoveryError });
    try {
      const devices = await this.hid.requestDevice({
        filters: PRODUCT_IDS.map(productId => ({ ...DEVICE_FILTER, productId })),
      });
      if (!devices.length) return;
      requireValid(devices.length === 1 && matchesPicoForge(devices[0]), "device selection");
      await this.attachDevice(devices[0]);
    } catch (error) { this.update({ error: error.message }); }
    finally { this.update({ busy: false }); }
  }

  async attachDevice(device) {
    const generation = this.generation;
    this.update({ busy: true });
    try {
      if (!device.opened) await device.open();
      if (generation !== this.generation) throw new Error("Connection cancelled");
      this.device = device;
      const hardware = decodeIdentityReport(await withTimeout(device.receiveFeatureReport(1), this.timeout));
      const expectedUid = this.reconnectUid;
      if (expectedUid && hardware.serial_number !== expectedUid) {
        await device.close();
        if (generation === this.generation && device === this.device) {
          this.device = null;
          this.update({ busy: false, reconnecting: true });
        }
        return;
      }
      const settings = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), this.timeout), hardware);
      if (generation !== this.generation || device !== this.device) throw new Error("Connection cancelled");
      this.reconnectUid = hardware.serial_number;
      device.addEventListener("inputreport", this.onInputReport);
      this.update({ ...EMPTY, connected: true, reconnecting: false, busy: true,
        outcome_uncertain: Boolean(this.recoveryError || this.outstanding || this.pendingWirelessCommand),
        error: this.recoveryError,
        hardware, settings, settings_supported: Boolean(hardware.capabilities & CAP.SETTINGS),
        msc_command_pending: this.mscPendingEnabled !== null });
      this.scheduleExpiry();
      if (this.outstanding) {
        const request = this.outstanding;
        try {
          const result = await this.pollResult(device, request);
          const succeeded = result.status === 2;
          if (succeeded && request.settings && (!sameSettings(result.settings, request.settings)
            || result.commit_mask !== request.commit_mask)) {
            throw new Error("Firmware did not confirm the selected values after reconnect");
          }
          this.retire(request, succeeded);
          this.update({ outcome_uncertain: false, error: succeeded ? null
            : `Command ${result.opcode}: status ${result.status}, error ${result.error_reason}`,
          recovered_opcode: succeeded ? result.opcode : null, recovery_message: !succeeded ? null
            : result.opcode === OP.CANCEL_TEST ? "Wireless test cancellation confirmed after reconnect."
              : "Pending command result confirmed after reconnect." });
          this.forcedSave?.settle({ result });
        } catch (error) {
          this.update({ outcome_uncertain: true, error: error.message });
          this.forcedSave?.settle({ error });
        }
      }
      if (this.pendingWirelessCommand && !this.outstanding) {
        const pending = this.pendingWirelessCommand;
        try {
          const outcome = await waitLifecycle(this, pending);
          if (generation !== this.generation || device !== this.device) {
            throw new Error("Wireless command outcome uncertain after disconnect");
          }
          this.settleWireless();
          this.update({ outcome_uncertain: Boolean(this.recoveryError),
            error: this.recoveryError ?? (outcome.kind === "failed" ? outcome.message : null),
            recovery_message: outcome.kind === "completed" ? "Wireless operation confirmed after reconnect." : null });
        } catch (error) {
          this.update({ outcome_uncertain: true, error: error.message });
        }
      }
      if (generation === this.generation && device === this.device) this.update({ busy: false });
    } catch (error) {
      if (device.opened) { try { await device.close(); } catch {} }
      if (generation === this.generation) {
        this.clearConnection();
        this.update({ reconnecting: false, error: error.message });
      }
    }
  }

  async disconnect() {
    if (this.state.busy) return;
    const device = this.device;
    if (!this.outstanding && !this.pendingWirelessCommand) this.reconnectUid = null;
    this.mscPendingEnabled = null;
    this.clearConnection();
    this.update({ reconnecting: false, error: this.recoveryError });
    if (device?.opened) {
      try { await device.close(); } catch (error) { this.update({ error: error.message }); }
    }
  }

  async pollResult(device, request) {
    const deadline = this.now() + this.timeout;
    const generation = this.generation;
    for (let attempt = 0; attempt < Math.ceil(this.timeout / 50) && this.now() < deadline; attempt++) {
      if (device !== this.device) throw new Error("Command outcome uncertain after disconnect");
      const report = await withTimeout(device.receiveFeatureReport(20), deadline - this.now());
      if (device !== this.device || generation !== this.generation) {
        throw new Error("Command outcome uncertain after disconnect");
      }
      const result = decodeControlResult(report, { ble: this.state.link === "ble" });
      if (result.transaction_id === request.transaction_id && result.opcode === request.opcode && result.status > 1) {
        if (result.status === 2 && request.opcode === OP.SCAN_ENTRY
          && ((result.data[0] | result.data[1] << 8) !== request.scan_generation
            || result.data[2] !== request.scan_index)) {
          throw new Error("Scan entry did not match the requested generation and index");
        }
        return result;
      }
      await this.sleep(50);
    }
    throw new Error("Command outcome uncertain: result timeout; disconnect/reconnect to read it, never replay");
  }

  /** @brief Check a console challenge against this board, then return its link-specific 16-byte proof. */
  async authorizeConsole(nonce, uid) {
    if (!(nonce instanceof Uint8Array) || nonce.length !== 16
      || !(uid instanceof Uint8Array) || uid.length !== 8) {
      throw new Error("Invalid TCP CLI authentication challenge");
    }
    const serial = this.state.connected ? this.state.hardware?.serial_number : null;
    if (serial) {
      const expectedUid = hexToBytes(serial);
      if (uid.some((byte, index) => byte !== expectedUid[index])) {
        throw new Error("The Wi-Fi console challenge belongs to a different board");
      }
    }
    return this.tcpCliAuthProof(nonce, uid);
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
   * Opcode-specific follow-up state belongs to the caller (setMsc, setSelfTest, reboot, saveSettings).
   */
  async command(opcode, options) {
    this.expireReports();
    const { blocked } = deriveOperationEligibility(this.state, {
      wirelessBusy: Boolean(this.wirelessOperation) && options?.workflow !== this.wirelessOperation });
    if (!this.device || blocked || this.outstanding || this.pendingWirelessCommand
      || this.state.self_test_command_pending || this.state.msc_command_pending) {
      throw new Error("Command unavailable or previous outcome uncertain");
    }
    const bit = opcode >= OP.STAGE_SSID ? CAP.WIRELESS : REQUIRED_CAP[opcode];
    if (bit && !(this.state.hardware.capabilities & bit)) throw new Error("Unavailable capability");
    const transaction = this.transaction % 255 + 1;
    const report = buildControlRequest(transaction, opcode, options);
    // A forced save detaches MSC and may reset USB; attachDevice settles this with the result read after reconnect.
    const forced = opcode === OP.SAVE_SETTINGS && report[3] & SAVE_SETTINGS_FORCE_MSC ? {} : null;
    if (forced) forced.done = new Promise(resolve => { forced.settle = outcome => resolve(forced.outcome = outcome); });
    this.forcedSave = forced;
    this.transaction = transaction;
    const request = { transaction_id: transaction, opcode };
    if (opcode === OP.SCAN_ENTRY) {
      request.scan_index = report[4];
      request.scan_generation = report[5] | report[6] << 8;
    }
    if (options?.lifecycle) request.lifecycle = options.lifecycle;
    if (opcode === OP.SAVE_SETTINGS) {
      request.settings = decodeSettings(report.subarray(4, 8));
      request.commit_mask = options?.commitMask ?? 0;
    }
    const device = this.device;
    const generation = this.generation;
    this.outstanding = request;
    if (opcode === OP.MSC) this.mscPendingEnabled = report[4] === 1;
    try { this.persistPendingOperation(); }
    catch (error) {
      this.outstanding = null;
      this.mscPendingEnabled = null;
      throw error;
    }
    this.update({ busy: true, error: null, recovery_message: null, recovered_opcode: null,
      ...(opcode === OP.MSC ? { msc_command_pending: true } : {}) });
    let sent = false;
    try {
      await withTimeout(device.sendFeatureReport(20, report), this.timeout);
      sent = true;
      if (generation !== this.generation) throw new Error("Command outcome uncertain after disconnect");
      const result = await this.pollResult(device, request);
      if (result.status !== 2) {
        this.retire(request, false);
        throw new Error(`Command ${opcode}: status ${result.status}, error ${result.error_reason}, `
          + `detail ${result.detail}`);
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
        && (this.outstanding || forced.outcome)) {
        const { result } = forced.outcome ?? await withTimeout(forced.done, 5000).catch(() => ({}));
        if (result?.transaction_id === transaction && result.opcode === opcode) {
          if (result.status !== 2) {
            throw new Error(`Command ${opcode}: status ${result.status}, error ${result.error_reason}`);
          }
          if (sameSettings(result.settings, request.settings) && result.commit_mask === request.commit_mask) {
            this.update({ settings: result.settings, outcome_uncertain: false });
            return result;
          }
        }
        if (this.state.error) throw new Error(this.state.error);
      }
      // SET rejection is authoritative: do not poll or accept a prior result.
      if (!sent && error.name !== "TimeoutError" && generation === this.generation) {
        try { this.retire(request, false); }
        catch (storageError) {
          this.update({
            error: `Command was not sent, but recovery state could not be cleared: ${storageError.message}` });
        }
      }
      if (generation === this.generation) {
        this.update({ outcome_uncertain: Boolean(this.outstanding || this.pendingWirelessCommand),
          error: error.message });
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
    this.expireReports();
    if (!this.state.runtime?.led_available) throw new Error("Fresh LED telemetry unavailable");
    return this.command(OP.LED, { args: [Number(on)] });
  }

  async reboot(confirmed = false) {
    if (confirmed !== true) throw new Error("Reboot requires explicit confirmation");
    if (!this.eligibility().storageReady || this.state.msc.enabled) {
      throw new Error("Fresh idle firmware-owned storage required");
    }
    const result = await this.command(OP.REBOOT, { flags: 1 });
    this.update({ reboot_pending: true });
    return result;
  }

  async setMsc(enable, force = false) {
    requireValid(typeof enable === "boolean" && typeof force === "boolean" && !(force && enable), "MSC action");
    if (!this.eligibility().storageReady || (force && !this.state.msc.enabled)) {
      throw new Error("Fresh idle storage status required");
    }
    return this.command(OP.MSC, { flags: Number(force), args: [force ? 2 : Number(enable)] });
  }

  async setSelfTest(action, sizeMiB = 32) {
    requireValid(["start", "cancel"].includes(action), "self-test action");
    if (!this.eligibility().storageFresh || this.state.msc.enabled || !this.state.self_test?.valid_for_control
      || (action === "start" && (this.state.self_test.active || !this.state.self_test.volume_mounted
        || !this.state.memory?.initialized))) throw new Error("Fresh self-test and firmware-owned storage required");
    if (action === "cancel") sizeMiB = 0;
    requireValid(Number.isInteger(sizeMiB) && sizeMiB >= (action === "start" ? 1 : 0) && sizeMiB <= 1024,
      "self-test size");
    const size = new Uint8Array(4);
    new DataView(size.buffer).setUint32(0, sizeMiB, true);
    const result = await this.command(OP.SELF_TEST, { args: [Number(action === "start"), ...size] });
    this.selfTestPendingSequence = this.state.self_test?.sequence ?? null;
    this.update({ self_test_command_pending: true });
    return result;
  }

  /** @brief Save next-boot settings; throws on refusal or failure, and marks the board as rebooting on success. */
  async saveSettings(settings, force = false, tcpCliEnabled) {
    requireValid(validSettings(settings), "boot settings");
    requireValid(typeof force === "boolean", "forced save confirmation");
    requireValid(tcpCliEnabled === undefined || typeof tcpCliEnabled === "boolean", "TCP CLI setting");
    if (!this.eligibility().storageReady || this.state.msc.enabled !== force) {
      throw new Error("Fresh idle firmware-owned storage required before saving.");
    }
    const tcp = tcpCliEnabled ?? settings.tcp_cli_enabled ?? this.state.settings?.tcp_cli_enabled ?? false;
    const result = await this.command(OP.SAVE_SETTINGS, { flags: Number(force), args: encodeSettings(settings, tcp) });
    this.update({ reboot_pending: true });
    return result;
  }
}
