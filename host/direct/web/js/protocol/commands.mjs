/**
 * @file Command table: the single source of each opcode's capability requirement and permitted links.
 */
import { BLE_OPCODES, CAP, OP } from "./constants.mjs";

/** @brief Capability bit each opcode requires; every wireless opcode requires CAP.WIRELESS. */
const REQUIRED_CAPABILITY = Object.freeze({
  [OP.LED]: CAP.LED, [OP.REBOOT]: CAP.REBOOT, [OP.SELF_TEST]: CAP.SELF_TEST, [OP.SAVE_SETTINGS]: CAP.SETTINGS,
});

/** @brief Capability bit an opcode requires, or 0 when the base firmware always provides it. */
export function requiredCapability(opcode) {
  return opcode >= OP.STAGE_SSID ? CAP.WIRELESS : REQUIRED_CAPABILITY[opcode] ?? 0;
}

/**
 * @brief Explain why a command cannot be sent over a link, or return null when it can.
 * @param {"usb" | "ble"} link Active transport kind.
 * @param {number} opcode Control opcode.
 * @param {number} [flags] Request flags byte.
 */
export function linkRestriction(link, opcode, flags = 0) {
  const bleOpcodes = /** @type {readonly number[]} */ (BLE_OPCODES);
  if (link === "ble" && (!bleOpcodes.includes(opcode) || opcode === OP.SAVE_SETTINGS && flags !== 0)) {
    return "This operation requires USB.";
  }
  return null;
}
