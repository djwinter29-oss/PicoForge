/**
 * @file Bluetooth key draft: generation, clipboard feedback and secret disposal. No board I/O.
 */
import { bytesToHex } from "../../protocol/codec.mjs";
import { queryElement } from "../dom.mjs";
import { describe } from "../messages.mjs";

/**
 * @brief Bind the Bluetooth key dialog; the wireless coordinator owns readiness and the draft lifecycle.
 * @param {{signal: () => AbortSignal, configured: () => boolean, enable: () => void, changed: () => void,
 *   clipboard: Pick<Clipboard, "writeText"> | null}} options
 */
export function bindBleKeyForm({ signal, configured, enable, changed, clipboard }) {
  const secret = queryElement("#direct-ble-secret");
  const status = queryElement("#direct-ble-status");
  const dialog = queryElement("#direct-ble-dialog");
  let dirty = false;
  let needsRegeneration = false;
  /** Incremented whenever the key draft changes; clipboard feedback must refer to that exact draft. */
  let revision = 0;
  const clear = () => {
    revision++;
    secret.value = "";
    secret.type = "password";
    dirty = false;
  };
  queryElement("#direct-ble-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    secret.value = bytesToHex(bytes);
    bytes.fill(0);
    revision++;
    secret.type = "text";
    dirty = true;
    needsRegeneration = false;
    enable();
    status.textContent = "Copy this key now. The board cannot show it again after saving.";
    changed();
  });
  queryElement("#direct-ble-copy").addEventListener("click", async () => {
    const lifecycle = signal();
    const copiedRevision = revision;
    const current = () => !lifecycle.aborted && copiedRevision === revision;
    try {
      if (!clipboard) throw new Error("Clipboard is unavailable");
      await clipboard.writeText(secret.value);
      if (current()) status.textContent = "Key copied. Save&Reboot to apply it.";
    } catch (error) {
      if (current()) status.textContent = `Could not copy key: ${describe(error)}`;
    }
  });
  queryElement("#direct-ble-cancel").addEventListener("click", () => dialog.close());
  return {
    dirty: () => dirty,
    canSave: () => !needsRegeneration,
    clear,
    open() {
      status.textContent = configured()
        ? "A Control key is saved. It cannot be read back; generate a replacement only if needed."
        : "No Control key is saved. Bluetooth stays off until a key is saved.";
      dialog.showModal();
    },
    /** @brief Return a replacement, an explicit removal, or undefined to retain the saved key. */
    /** @param {boolean} enabled @return {string | undefined} */
    request(enabled) {
      return enabled && dirty ? secret.value : !enabled && configured() ? "" : undefined;
    },
    /** @brief Wipe submitted key text on every outcome; a failed replacement must be regenerated. */
    /** @param {boolean} saved @param {string} [error] */
    finish(saved, error) {
      needsRegeneration = !saved && dirty;
      clear();
      status.textContent = saved ? "" : error;
    },
    reset() {
      needsRegeneration = false;
      clear();
      if (dialog.open) dialog.close();
    },
  };
}
