// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * @file Bluetooth key draft: generation, clipboard feedback and secret disposal. No board I/O.
 */
import { bytesToHex } from "../../protocol/codec.mjs";
import { queryElement, setText } from "../dom.mjs";
import { describe } from "../messages.mjs";

/**
 * @brief Bind the Bluetooth key dialog; the wireless coordinator owns readiness and the draft lifecycle.
 * @param {{signal: () => AbortSignal, configured: () => boolean, enable: () => void, changed: () => void,
 *   clipboard: Pick<Clipboard, "writeText"> | null}} options
 */
export function bindBleKeyForm({ signal, configured, enable, changed, clipboard }) {
  /** @type {HTMLInputElement} */
  const secret = queryElement("input#direct-ble-secret");
  /** @type {HTMLDialogElement} */
  const dialog = queryElement("dialog#direct-ble-dialog");
  let dirty = false;
  let needsRegeneration = false;
  /** Incremented whenever the key draft changes; clipboard feedback must refer to that exact draft. */
  let revision = 0;
  const saveBlockReason = () => needsRegeneration
    ? "Generate a replacement Control key or disable Bluetooth before saving." : null;
  const clear = () => {
    revision++;
    secret.value = "";
    secret.type = "password";
    dirty = false;
    needsRegeneration = false;
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
    setText({ "direct-ble-status": "Copy this key now. The board cannot show it again after saving." });
    changed();
  });
  queryElement("#direct-ble-copy").addEventListener("click", async () => {
    const lifecycle = signal();
    const copiedRevision = revision;
    const current = () => !lifecycle.aborted && copiedRevision === revision;
    try {
      if (!clipboard) throw new Error("Clipboard is unavailable");
      await clipboard.writeText(secret.value);
      if (current()) setText({ "direct-ble-status": "Key copied. Save&Reboot to apply it." });
    } catch (error) {
      if (current()) setText({ "direct-ble-status": `Could not copy key: ${describe(error)}` });
    }
  });
  queryElement("#direct-ble-cancel").addEventListener("click", () => dialog.close());
  return {
    dirty: () => dirty,
    canSave: () => saveBlockReason() === null,
    saveBlockReason,
    clear,
    open() {
      setText({ "direct-ble-status": configured()
        ? "A Control key is saved. It cannot be read back; generate a replacement only if needed."
        : "No Control key is saved. Bluetooth stays off until a key is saved." });
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
      const failedReplacement = !saved && dirty;
      clear();
      needsRegeneration = failedReplacement;
      setText({ "direct-ble-status": saved ? "" : error });
    },
    reset() {
      needsRegeneration = false;
      clear();
      if (dialog.open) dialog.close();
    },
  };
}
