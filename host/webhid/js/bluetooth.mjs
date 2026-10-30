import { PicoForgeWebHid } from "./device.mjs?v=4";

export const BLE_SERVICE = "50fc0000-7c91-4d70-8b32-6e856443f501";
const characteristic = id => `50fc${id.toString(16).padStart(4, "0")}-7c91-4d70-8b32-6e856443f501`;

/** Web Bluetooth uses the existing report decoder/transaction engine, but never reconnects or replays writes. */
export class PicoForgeBluetooth extends PicoForgeWebHid {
  constructor(bluetooth, onChange, options) {
    super(undefined, onChange, options);
    this.bluetooth = bluetooth;
    this.characteristics = new Map();
    this.pollTimer = null;
    this.bleKey = null;
  }

  async connect() {
    if (!this.bluetooth || this.state.busy || this.state.connected) return;
    this.update({ busy: true, error: null });
    try {
      this.bleDevice = await this.bluetooth.requestDevice({ filters: [{ services: [BLE_SERVICE] }] });
      this.bleDevice.addEventListener("gattserverdisconnected", () => {
        clearTimeout(this.pollTimer);
        this.bleKey = null;
        this.characteristics.clear();
        this.clearConnection();
        this.update({ reconnecting: false, error: this.outstanding
          ? "BLE command outcome uncertain; inspect status before any retry." : null });
      });
      const server = await this.bleDevice.gatt.connect();
      const service = await server.getPrimaryService(BLE_SERVICE);
      for (const id of [1, 2, 3, 10, 11, 20, 30, 32, 33]) {
        this.characteristics.set(id, await service.getCharacteristic(characteristic(id)));
      }
      const adapter = new EventTarget();
      Object.assign(adapter, { opened: true, open: async () => {},
        close: async () => this.bleDevice.gatt.disconnect(),
        receiveFeatureReport: async id => this.characteristics.get(id).readValue(),
        sendFeatureReport: async (id, report) => {
          if (id !== 20 || ![1, 4].includes(report[2])) throw new Error("This operation requires USB.");
          if (!this.bleKey) throw new Error("Enter the USB-provisioned BLE control key first.");
          const nonce = new Uint8Array(await this.characteristics.get(32).readValue().then(value =>
            value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength)));
          if (nonce.length !== 16) throw new Error("Invalid BLE authorization challenge");
          const uid = Uint8Array.from(this.state.hardware.serial_number.match(/../g), byte => parseInt(byte, 16));
          const message = new Uint8Array([...nonce, ...uid, ...report]);
          const mac = new Uint8Array(await crypto.subtle.sign("HMAC", this.bleKey, message));
          await this.characteristics.get(33).writeValueWithResponse(mac.slice(0, 16));
          await this.characteristics.get(20).writeValueWithResponse(report);
        } });
      await this.attachDevice(adapter);
      this.reconnectUid = null;
      if (!this.state.connected) throw new Error(this.state.error || "BLE connection initialization failed");
      this.update({ transport: "ble" });
      await this.pollReports();
    } catch (error) {
      this.bleDevice?.gatt.disconnect();
      this.update({ error: error.message, busy: false });
    } finally { this.update({ busy: false }); }
  }

  async setControlKey(hex) {
    if (!/^[a-f0-9]{64}$/i.test(hex) || /^0+$/.test(hex)) throw new Error("BLE key must be 64 nonzero hex digits.");
    const bytes = Uint8Array.from(hex.match(/../g), byte => parseInt(byte, 16));
    try { this.bleKey = await crypto.subtle.importKey("raw", bytes, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]); }
    finally { bytes.fill(0); }
  }

  async pollReports() {
    if (!this.device || !this.state.connected) return;
    if (!this.state.busy && !this.outstanding) {
      try {
        for (const id of [3, 10, ...(this.state.hardware.capabilities & 32 ? [11] : [])]) {
          const device = this.device;
          const data = await this.characteristics.get(id).readValue();
          this.onInputReport({ device, reportId: id, data });
        }
      } catch (error) { this.update({ error: `BLE status read failed: ${error.message}` }); }
    }
    if (this.state.connected) this.pollTimer = setTimeout(() => this.pollReports(), 250);
  }

  async disconnect() {
    if (this.state.busy) return;
    clearTimeout(this.pollTimer);
    this.bleKey = null;
    await super.disconnect();
  }
}
