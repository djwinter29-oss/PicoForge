/**
 * Wireless workflows run inside BoardSession.runWirelessWorkflow and use only its context (command, read,
 * finishLifecycle, invalidateWireless, savedSsidHex, settings); never session fields, a transport or the DOM.
 */
import {
  buildControlRequest, bytesToHex as hex, decodeScanEntry, displaySsid, encodeSettings,
  parseHexBytes, ssidBytes, validSettings,
} from "../../protocol/codec.mjs";
import { OP, STAGED_EXPIRED } from "../../protocol/constants.mjs";

async function stage(context, opcode, bytes, signal) {
  try { return await context.command(opcode, { args: [bytes.length, ...bytes], signal }); }
  finally { bytes.fill(0); }
}

function passwordBytes(password) {
  if (typeof password !== "string" || password !== "" && !/^[\x20-\x7e]{8,63}$/.test(password)) {
    throw new Error("Password must contain 8-63 printable ASCII characters or be empty for an open network");
  }
  return new TextEncoder().encode(password);
}

async function stagePair(context, settings, signal) {
  const ssid = ssidBytes(settings);
  const same = hex(ssid) === context.savedSsidHex();
  if (ssid.length && settings.password === undefined && !same) {
    throw new Error("A changed SSID requires an explicit password (empty for an open network)");
  }
  // Validate both fields before any write.
  const password = settings.password === undefined ? null : passwordBytes(settings.password);
  try {
    await stage(context, OP.STAGE_SSID, ssid, signal);
    if (password && ssid.length !== 0) await stage(context, OP.STAGE_PASSWORD, password, signal);
  } finally { ssid.fill(0); password?.fill(0); }
}

/** @brief Wait for an accepted lifecycle; a reported failure becomes an error. */
async function finish(context, pending, onProgress, signal) {
  const outcome = await context.finishLifecycle(pending, onProgress, signal);
  if (outcome.kind === "failed") throw new Error(outcome.message);
  return outcome.result;
}

export async function scanWireless(client, signal) {
  return client.runWirelessWorkflow(async context => {
    const previous = await context.read(signal);
    const pending = { operation: OP.SCAN, generation: previous.scan_generation % 65535 + 1 };
    await context.command(OP.SCAN, { lifecycle: pending, signal });
    const status = await finish(context, pending, undefined, signal);
    const networks = [];
    for (let index = 0; index < status.scan_count; index++) {
      networks.push(decodeScanEntry(await context.command(OP.SCAN_ENTRY, { signal,
        args: [index, status.scan_generation & 255, status.scan_generation >> 8] })));
    }
    return networks;
  });
}

export async function testWireless(client, settings, onProgress, signal) {
  return client.runWirelessWorkflow(async context => {
    const previous = await context.read(signal);
    if (!ssidBytes(settings).length) throw new Error("SSID is required for a connection test");
    await stagePair(context, settings, signal);
    const pending = { operation: OP.TEST_WIFI, generation: previous.test_generation % 255 + 1 };
    await context.command(OP.TEST_WIFI, { lifecycle: pending, signal });
    return finish(context, pending, onProgress, signal);
  });
}

export async function cancelWireless(client, signal) {
  return client.runWirelessWorkflow(async context => {
    await context.command(OP.CANCEL_TEST, { signal });
    try { await context.read(signal); }
    catch (error) { context.invalidateWireless(error.message); }
  });
}

export async function saveWireless(client, settings, signal) {
  return client.runWirelessWorkflow(async context => {
    const previous = await context.read(signal);
    const board = settings.board;
    if (typeof settings.wifi_enabled !== "boolean" || typeof settings.ble_enabled !== "boolean"
      || settings.tcp_cli_enabled !== undefined && typeof settings.tcp_cli_enabled !== "boolean"
      || settings.use_tested_wifi !== undefined && typeof settings.use_tested_wifi !== "boolean"
      || !validSettings(board)
      || board.radios !== (Number(settings.wifi_enabled) | Number(settings.ble_enabled) << 1)) {
      throw new Error("Invalid wireless configuration");
    }
    const tcp = settings.tcp_cli_enabled ?? context.settings().tcp_cli_enabled;
    const args = encodeSettings(board, tcp);
    buildControlRequest(1, OP.SAVE_SETTINGS, { args }); // Validate board settings before staging.
    const target = ssidBytes(settings);
    const targetHex = hex(target);
    const targetLabel = displaySsid(target);
    const key = settings.ble_key === "" ? new Uint8Array()
      : settings.ble_key === undefined ? null : parseHexBytes(settings.ble_key, 32, "Control key (64 hex digits)");
    if (settings.wifi_enabled && !target.length) throw new Error("Wi-Fi requires a network");
    if (settings.use_tested_wifi) {
      if (!previous.pair_tested || previous.staged_state & STAGED_EXPIRED || previous.ssid_hex !== targetHex
        || settings.password !== undefined) throw new Error("Tested Wi-Fi pair no longer matches; test again");
    } else if (targetHex !== previous.ssid_hex || settings.password !== undefined) {
      await stagePair(context, settings, signal);
    }
    try {
      if (key) await stage(context, OP.STAGE_KEY, key, signal);
    } finally { key?.fill(0); target.fill(0); }
    const staged = await context.read(signal);
    const result = await context.command(OP.SAVE_SETTINGS, { flags: staged.staged_mask ? 2 : 0,
      args, commitMask: staged.staged_mask, signal, reboots: true });
    return { ...result.settings, wifi_enabled: settings.wifi_enabled, ble_enabled: settings.ble_enabled,
      ssid: targetLabel, ssid_hex: targetHex, tcp_cli_enabled: tcp,
      ble_key_configured: settings.ble_key === undefined ? previous.ble_key_configured : Boolean(settings.ble_key) };
  });
}
