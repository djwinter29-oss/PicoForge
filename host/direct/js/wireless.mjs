/**
 * USB wireless workflows (Feature 12 codecs live in protocol.mjs). Imports only protocol.mjs so device.mjs can reuse
 * waitLifecycle for reconnect recovery without an import cycle.
 */
import {
  OP, buildControlRequest, bytesToHex as hex, decodeScanEntry, decodeWireless, displaySsid, encodeSettings,
  parseHexBytes, ssidBytes, validSettings, withTimeout,
} from "./protocol.mjs?v=47";

/** @brief Feature 12 poll interval: fast while a Wi-Fi test or scan runs, otherwise 2 s. */
export const wirelessPollInterval = wireless => wireless?.state === 4 || wireless?.scan_state === 1 ? 500 : 2000;

export async function readWireless(client, device = client.device) {
  const generation = client.generation;
  const wireless = decodeWireless(await withTimeout(device.receiveFeatureReport(12), 1500), client.state.settings);
  if (device !== client.device || generation !== client.generation) throw new Error("Wireless connection changed");
  client.recordWireless(wireless);
  return wireless;
}

async function stage(client, token, opcode, bytes) {
  try { return await client.command(opcode, { workflow: token, args: [bytes.length, ...bytes] }); }
  finally { bytes.fill(0); }
}

function passwordBytes(password) {
  if (typeof password !== "string" || password !== "" && !/^[\x20-\x7e]{8,63}$/.test(password)) {
    throw new Error("Password must contain 8-63 printable ASCII characters or be empty for an open network");
  }
  return new TextEncoder().encode(password);
}

async function stagePair(client, token, settings) {
  const ssid = ssidBytes(settings);
  const same = hex(ssid) === client.savedSsidHex;
  if (ssid.length && settings.password === undefined && !same) {
    throw new Error("A changed SSID requires an explicit password (empty for an open network)");
  }
  // Validate both fields before any write.
  const password = settings.password === undefined ? null : passwordBytes(settings.password);
  try {
    await stage(client, token, OP.STAGE_SSID, ssid);
    if (password && ssid.length !== 0) await stage(client, token, OP.STAGE_PASSWORD, password);
  } finally { ssid.fill(0); password?.fill(0); }
}

function lifecycleOutcome(status, pending) {
  if (pending.operation === OP.SCAN) {
    if (status.scan_generation !== pending.generation) throw new Error("Wireless scan generation changed");
    if (status.scan_state === 3) return { kind: "failed", message: "Wireless scan failed" };
    if (status.scan_state === 2) return { kind: "completed", result: status };
  } else {
    if (status.test_generation !== pending.generation) throw new Error("Wireless test generation changed");
    if (status.state === 6 || status.staged_state & 8) {
      return { kind: "failed", message: status.phase || "Test credentials expired" };
    }
    if (status.state === 5 && status.pair_tested) {
      // ponytail: no lifetime is transmitted; the board starts a 600s window on success. Subtract polling latency.
      return { kind: "completed", result: { ...status, remaining_seconds: 599 } };
    }
  }
  return null;
}

/** @brief Poll Feature 12 until the pending scan/test lifecycle completes or fails; never replays it. */
export async function waitLifecycle(client, pending, onProgress) {
  const device = client.device;
  const deadline = client.now() + (pending.operation === OP.TEST_WIFI ? 35000 : 15000);
  while (client.now() < deadline) {
    const status = await readWireless(client, device);
    const outcome = lifecycleOutcome(status, pending);
    if (outcome) return outcome;
    onProgress?.(status.phase || "Connecting");
    await client.sleep(500);
  }
  throw new Error("Wireless operation timed out; reconnect to inspect it, never replay");
}

async function finishLifecycle(client, pending, onProgress) {
  try {
    const outcome = await waitLifecycle(client, pending, onProgress);
    client.settleWireless();
    if (outcome.kind === "failed") throw new Error(outcome.message);
    return outcome.result;
  } catch (error) {
    if (client.pendingWirelessCommand) client.update({ outcome_uncertain: true, error: error.message });
    throw error;
  }
}

export async function scanWireless(client) {
  return client.exclusiveWireless(async token => {
    const previous = await readWireless(client);
    const pending = { operation: OP.SCAN, generation: previous.scan_generation % 65535 + 1 };
    await client.command(OP.SCAN, { workflow: token, lifecycle: pending });
    const status = await finishLifecycle(client, pending);
    const networks = [];
    for (let index = 0; index < status.scan_count; index++) {
      networks.push(decodeScanEntry(await client.command(OP.SCAN_ENTRY, { workflow: token,
        args: [index, status.scan_generation & 255, status.scan_generation >> 8] })));
    }
    return networks;
  });
}

export async function testWireless(client, settings, onProgress) {
  return client.exclusiveWireless(async token => {
    const previous = await readWireless(client);
    if (!ssidBytes(settings).length) throw new Error("SSID is required for a connection test");
    await stagePair(client, token, settings);
    const pending = { operation: OP.TEST_WIFI, generation: previous.test_generation % 255 + 1 };
    await client.command(OP.TEST_WIFI, { workflow: token, lifecycle: pending });
    return finishLifecycle(client, pending, onProgress);
  });
}

export async function cancelWireless(client) {
  return client.exclusiveWireless(async token => {
    await client.command(OP.CANCEL_TEST, { workflow: token });
    try { await readWireless(client); }
    catch (error) { client.update({ wireless: null, wireless_error: error.message }); }
  });
}

export async function saveWireless(client, settings) {
  return client.exclusiveWireless(async token => {
    const previous = await readWireless(client);
    const board = settings.board;
    if (typeof settings.wifi_enabled !== "boolean" || typeof settings.ble_enabled !== "boolean"
      || settings.tcp_cli_enabled !== undefined && typeof settings.tcp_cli_enabled !== "boolean"
      || settings.use_tested_wifi !== undefined && typeof settings.use_tested_wifi !== "boolean"
      || !validSettings(board)
      || board.radios !== (Number(settings.wifi_enabled) | Number(settings.ble_enabled) << 1)) {
      throw new Error("Invalid wireless configuration");
    }
    const tcp = settings.tcp_cli_enabled ?? client.state.settings.tcp_cli_enabled;
    const args = encodeSettings(board, tcp);
    buildControlRequest(1, OP.SAVE_SETTINGS, { args }); // Validate board settings before staging.
    const target = ssidBytes(settings);
    const targetHex = hex(target);
    const targetLabel = displaySsid(target);
    const key = settings.ble_key === "" ? new Uint8Array()
      : settings.ble_key === undefined ? null : parseHexBytes(settings.ble_key, 32, "Control key (64 hex digits)");
    if (settings.wifi_enabled && !target.length) throw new Error("Wi-Fi requires a network");
    if (settings.use_tested_wifi) {
      if (!previous.pair_tested || previous.staged_state & 8 || previous.ssid_hex !== targetHex
        || settings.password !== undefined) throw new Error("Tested Wi-Fi pair no longer matches; test again");
    } else if (targetHex !== previous.ssid_hex || settings.password !== undefined) {
      await stagePair(client, token, settings);
    }
    try {
      if (key) await stage(client, token, OP.STAGE_KEY, key);
    } finally { key?.fill(0); target.fill(0); }
    const staged = await readWireless(client);
    const result = await client.command(OP.SAVE_SETTINGS, { workflow: token, flags: staged.staged_mask ? 2 : 0,
      args, commitMask: staged.staged_mask });
    client.update({ reboot_pending: true });
    return { ...result.settings, wifi_enabled: settings.wifi_enabled, ble_enabled: settings.ble_enabled,
      ssid: targetLabel, ssid_hex: targetHex, tcp_cli_enabled: tcp,
      ble_key_configured: settings.ble_key === undefined ? previous.ble_key_configured : Boolean(settings.ble_key) };
  });
}
