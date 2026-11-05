import { PicoForgeWebHid } from "./device.mjs?v=47";
import { BLE_OPCODES, CAP, OP, hexToBytes, parseHexBytes } from "./protocol.mjs?v=47";

export const BLE_SERVICE = "50fc0000-7c91-4d70-8b32-6e856443f502";
const characteristic = id => `50fc${id.toString(16).padStart(4, "0")}-7c91-4d70-8b32-6e856443f502`;
const GATT_SERVICE = "generic_attribute";

/** Web Bluetooth uses the existing report decoder/transaction engine, but never reconnects or replays writes. */
export class PicoForgeBluetooth extends PicoForgeWebHid {
  constructor(bluetooth, onChange, options) {
    super(undefined, onChange, options);
    this.bluetooth = bluetooth;
    this.characteristics = new Map();
    this.pollTimer = null;
    this.bleKey = null;
    this.state = { ...this.state, link: "ble",
      error: bluetooth ? null : "Web Bluetooth is unavailable in this browser." };
    this.connecting = false;
    // Stable reference: browsers reuse one BluetoothDevice per board, so repeated scans must not stack handlers.
    this.onBleDisconnected = () => {
      // Setup failures report their own error; a late event must not erase it or cancel the next attempt.
      if (this.connecting || !this.state.connected) return;
      clearTimeout(this.pollTimer);
      this.bleKey = null;
      this.characteristics.clear();
      this.clearConnection();
      this.update({ reconnecting: false, error: this.outstanding
        ? "BLE command outcome uncertain; inspect status before any retry." : null });
    };
  }

  /** Sign request with the control key over a fresh nonce and write the 16-byte proof; uid is the raw board ID. */
  async prove(uid, request) {
    const value = await this.characteristics.get(256).readValue();
    const nonce = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    if (nonce.length !== 16) {
      throw Object.assign(new Error("The board did not provide a fresh BLE challenge; reconnect and retry."),
        { final: true });
    }
    const message = new Uint8Array([...nonce, ...uid, ...new TextEncoder().encode("PFBLECTL1"), ...request]);
    const mac = new Uint8Array(await crypto.subtle.sign("HMAC", this.bleKey, message));
    await this.characteristics.get(257).writeValueWithResponse(mac.slice(0, 16));
  }

  /** The board refuses every read but identity until the connection proves the key once. */
  async unlock() {
    const value = await this.characteristics.get(1).readValue();
    const uid = new Uint8Array(value.buffer.slice(value.byteOffset + 3, value.byteOffset + 11));
    try { await this.prove(uid, new TextEncoder().encode("SESSION01")); }
    catch (error) {
      if (error.final) throw error;
      if (!this.bleDevice.gatt.connected) throw error;
      throw Object.assign(new Error("The board rejected the unlock proof. Check the Control key or wait 2 s."),
        { final: true });
    }
  }

  async tcpCliAuthProof(nonce, uid) {
    if (!this.state.connected || !this.bleKey || !this.bleDevice?.gatt.connected) {
      throw new Error("Connect the matching PicoForge board over Bluetooth to authorize its Wi-Fi console");
    }
    const message = new Uint8Array(16 + 8 + 9 + 9);
    message.set(nonce);
    message.set(uid, 16);
    message.set(new TextEncoder().encode("PFTCPCLI1"), 24);
    message.set(new TextEncoder().encode("TCPCLI001"), 33);
    const mac = new Uint8Array(await crypto.subtle.sign("HMAC", this.bleKey, message));
    return mac.slice(0, 16);
  }

  async connect(hex) {
    if (!this.bluetooth || this.state.busy || this.state.connected) return;
    this.update({ busy: true, error: null });
    this.connecting = true;
    try {
      await this.setControlKey(hex);
      this.bleDevice = await this.bluetooth.requestDevice({
        filters: [{ services: [BLE_SERVICE] }, { namePrefix: "PicoForge-" }],
        optionalServices: [BLE_SERVICE, GATT_SERVICE],
      });
      this.bleDevice.addEventListener("gattserverdisconnected", this.onBleDisconnected);
      const adapter = this.adapter();
      if (this.outstanding && this.bleUid) this.reconnectUid = this.bleUid;
      // ponytail: the first link to a newly permitted board often fails GATT discovery or the first read; later
      // links use the browser's cache. Only read-only setup is retried, a fixed 3 times, on the chosen device.
      for (let attempt = 1; ; attempt++) {
        try {
          const server = await this.bleDevice.gatt.connect();
          try {
            const gatt = await server.getPrimaryService(GATT_SERVICE);
            await (await gatt.getCharacteristic("service_changed")).startNotifications();
          } catch (error) {
            if (!["NotAllowedError", "NotFoundError", "NotSupportedError", "SecurityError"].includes(error?.name)) {
              throw error;
            }
            console.warn("Service Changed is unavailable; continuing with platform GATT cache behavior.", error);
          }
          const service = await server.getPrimaryService(BLE_SERVICE);
          for (const id of [1, 2, 3, 10, 11, 12, 20, 256, 257]) {
            this.characteristics.set(id, await service.getCharacteristic(characteristic(id)));
          }
          await this.unlock();
          await this.attachDevice(adapter);
          if (!this.state.connected) throw new Error(this.state.error || "BLE connection initialization failed");
          break;
        } catch (error) {
          this.bleDevice.gatt.disconnect();
          if (attempt === 3 || error.final) throw error;
          await this.sleep(500);
        }
      }
      this.bleUid = this.state.hardware.serial_number;
      this.connecting = false;
      if (!this.bleDevice.gatt.connected) return this.onBleDisconnected();
      await this.pollReports();
    } catch (error) {
      this.bleDevice?.gatt.disconnect();
      this.bleKey = null;
      this.characteristics.clear();
      this.update({ error: error.message, busy: false });
    } finally {
      this.connecting = false;
      this.update({ busy: false });
    }
  }

  /**
   * @brief Present the GATT link as the HID-like device attachDevice and command expect. Writes other than the LED
   * carry a fresh key proof; command() already restricted the opcode to BLE_OPCODES.
   */
  adapter() {
    return Object.assign(new EventTarget(), { opened: true, open: async () => {},
      close: async () => this.bleDevice.gatt.disconnect(),
      receiveFeatureReport: async id => {
        const value = await this.characteristics.get(id).readValue();
        if (id !== 20) return value;
        if (value.byteLength !== 9) throw new Error("Expected exactly 9 BLE result bytes");
        const report = new Uint8Array(68);
        report.set(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
        return report;
      },
      sendFeatureReport: async (id, report) => {
        if (id !== 20 || report.length !== 68 || report.subarray(9).some(Boolean)
          || report[2] === OP.SAVE_SETTINGS && report[3] !== 0) {
          throw new Error("This operation requires USB.");
        }
        const request = report.slice(0, 9);
        // The LED only identifies the board, so firmware accepts it without a proof.
        if (report[2] === OP.LED) return this.characteristics.get(20).writeValueWithResponse(request);
        try {
          await this.prove(hexToBytes(this.state.hardware.serial_number), request);
          await this.characteristics.get(20).writeValueWithResponse(request);
        } catch (error) {
          // Firmware answers a bad, missing or rate-limited proof with an ATT application error.
          if (error.final) throw error;
          if (!this.bleDevice.gatt.connected) throw error;
          throw new Error("The board rejected or rate-limited the proof. Check the Control key or wait 2 s.");
        }
      } });
  }

  async setControlKey(hex) {
    const bytes = parseHexBytes(hex ?? "", 32, "Control key (64 hex digits)");
    try {
      this.bleKey = await crypto.subtle.importKey("raw", bytes, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    }
    finally { bytes.fill(0); }
  }

  async command(opcode, options) {
    if (!BLE_OPCODES.includes(opcode)) throw new Error("This operation requires USB.");
    return super.command(opcode, options);
  }

  async pollReports() {
    if (!this.device || !this.state.connected) return;
    if (!this.state.busy && !this.outstanding) {
      try {
        for (const id of [3, 10, ...(this.state.hardware.capabilities & CAP.SELF_TEST ? [11] : [])]) {
          const device = this.device;
          const data = await this.characteristics.get(id).readValue();
          this.onInputReport({ device, reportId: id, data });
        }
      } catch (error) { this.update({ error: `BLE status read failed: ${error.message}` }); }
    }
    if (this.state.connected) this.pollTimer = setTimeout(() => this.pollReports(), 250);
  }

  dispose() {
    clearTimeout(this.pollTimer);
    this.bleDevice?.removeEventListener("gattserverdisconnected", this.onBleDisconnected);
    super.dispose();
  }

  async disconnect() {
    if (this.state.busy) return;
    clearTimeout(this.pollTimer);
    this.bleKey = null;
    this.characteristics.clear();
    await super.disconnect();
  }
}
