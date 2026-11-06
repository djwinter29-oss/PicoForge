// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { DEVICE_FILTER, PRODUCT_IDS } from "../protocol/constants.mjs";
import { matchesPicoForge, requireValid } from "../protocol/codec.mjs";
import { BoardError, REASON } from "../state/errors.mjs";

/** @typedef {import("./transport.mjs").Transport} Transport */
/** @implements {Transport} */
export class HidTransport extends EventTarget {
  constructor(hid) {
    super();
    /** @type {"usb"} */
    this.kind = "usb";
    this.api = hid;
    this.available = Boolean(hid);
    this.unavailableReason = REASON.WEBHID_UNAVAILABLE;
    this.autoReconnect = true;
    this.pollInterval = null;
    this.device = null;
    this.candidate = null;
    this.name = null;
    this.onReport = event => {
      if (event.device === this.device) this.dispatchEvent(Object.assign(new Event("report"),
        { reportId: event.reportId, data: event.data }));
    };
    this.onDisconnect = event => {
      if (event.device !== this.device) return;
      this.detach();
      this.dispatchEvent(new Event("disconnect"));
    };
    this.onConnect = event => {
      if (this.device || !matchesPicoForge(event.device)) return;
      this.candidate = event.device;
      this.dispatchEvent(new Event("available"));
    };
    this.onSuspend = event => {
      if (event.device === this.device) this.dispatchEvent(new Event("suspend"));
    };
    hid?.addEventListener("disconnect", this.onDisconnect);
    hid?.addEventListener("connect", this.onConnect);
    hid?.addEventListener("suspend", this.onSuspend);
  }

  async connect(_options, signal) {
    signal.throwIfAborted();
    if (!this.api) throw new BoardError(this.unavailableReason);
    const devices = await this.api.requestDevice({
      filters: PRODUCT_IDS.map(productId => ({ ...DEVICE_FILTER, productId })),
    });
    signal.throwIfAborted();
    if (!devices.length) return false;
    requireValid(devices.length === 1 && matchesPicoForge(devices[0]), "device-selection");
    await this.openDevice(devices[0], signal);
    return true;
  }

  async reconnect(signal) {
    const device = this.candidate;
    this.candidate = null;
    if (!device) return false;
    await this.openDevice(device, signal);
    return true;
  }

  async openDevice(device, signal) {
    signal.throwIfAborted();
    if (!device.opened) await device.open();
    if (signal.aborted) {
      await device.close();
      signal.throwIfAborted();
    }
    this.device = device;
    this.name = device.productName ?? null;
    device.addEventListener("inputreport", this.onReport);
  }

  async readReport(id, signal) {
    signal.throwIfAborted();
    const device = this.device;
    if (!device) throw new BoardError(REASON.USB_DISCONNECTED);
    const value = await device.receiveFeatureReport(id);
    signal.throwIfAborted();
    if (device !== this.device) throw new BoardError(REASON.USB_CHANGED);
    return value;
  }

  async writeReport(id, data, signal) {
    signal.throwIfAborted();
    const device = this.device;
    if (!device) throw new BoardError(REASON.USB_DISCONNECTED);
    await device.sendFeatureReport(id, data);
    signal.throwIfAborted();
    if (device !== this.device) throw new BoardError(REASON.USB_CHANGED);
  }

  /** @brief USB console proofs use the serialized board command queue rather than a transport-owned key. */
  async createConsoleProof(_nonce, _uid, signal) {
    signal.throwIfAborted();
    return null;
  }

  detach() {
    const device = this.device;
    device?.removeEventListener("inputreport", this.onReport);
    this.device = null;
    this.name = null;
    return device;
  }

  async disconnect() {
    const device = this.detach();
    if (device?.opened) await device.close();
  }

  async dispose() {
    this.api?.removeEventListener("disconnect", this.onDisconnect);
    this.api?.removeEventListener("connect", this.onConnect);
    this.api?.removeEventListener("suspend", this.onSuspend);
    this.candidate = null;
    await this.disconnect();
  }
}
