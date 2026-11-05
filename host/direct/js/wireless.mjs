import { buildControlRequest, withTimeout } from "./device.mjs?v=12";

const hex = bytes => [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
const allZero = bytes => bytes.every(byte => byte === 0);
const bytesOf = report => report instanceof DataView
  ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;
export const WIRELESS_TEST_DETAILS = ["", "Radio initialization failed", "Network not found", "Password rejected",
  "No DHCP address", "Timed out", "Test connection lost"];

/** Preserve opaque bytes separately from the printable, never round-tripped label. */
export function displaySsid(bytes) {
  let text = "";
  for (let index = 0; index < bytes.length;) {
    const byte = bytes[index];
    const size = byte < 128 ? 1 : byte >= 194 && byte <= 223 ? 2 : byte <= 239 && byte >= 224 ? 3
      : byte >= 240 && byte <= 244 ? 4 : 0;
    let decoded;
    try {
      if (!size || index + size > bytes.length) throw new Error();
      decoded = new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(index, index + size));
    } catch { decoded = null; }
    if (!decoded || /[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]/u.test(decoded)) {
      text += `\\x${byte.toString(16).padStart(2, "0").toUpperCase()}`;
      index++;
    } else { text += decoded; index += size; }
  }
  return text;
}

export function ssidBytes(settings) {
  if (settings.ssid_hex !== undefined) {
    if (typeof settings.ssid_hex !== "string" || !/^(?:[a-f0-9]{2}){0,32}$/i.test(settings.ssid_hex)) {
      throw new Error("SSID hex must encode at most 32 bytes");
    }
    return Uint8Array.from(settings.ssid_hex.match(/../g) ?? [], byte => parseInt(byte, 16));
  }
  if (typeof settings.ssid !== "string") throw new Error("SSID is required");
  const bytes = new TextEncoder().encode(settings.ssid);
  if (bytes.length > 32) throw new Error("SSID must be at most 32 UTF-8 bytes");
  return bytes;
}

export function decodeWireless(report, settings) {
  const bytes = bytesOf(report);
  if (!(bytes instanceof Uint8Array) || bytes.length !== 48 || bytes[0] !== 1 || bytes[1] > 6
    || bytes[3] > 2 || bytes[9] > 32 || !allZero(bytes.subarray(10 + bytes[9], 42))
    || bytes[42] > 3 || bytes[45] > 18 || bytes[42] !== 2 && bytes[45] !== 0
    || bytes[42] !== 0 && !(bytes[43] | bytes[44] << 8)
    || bytes[1] >= 4 && !bytes[46]
    || bytes[47] & 8 && bytes[4] & 240
    || bytes[47] & 1 && !(bytes[4] & 16) || bytes[47] & 2 && !(bytes[4] & 32)
    || bytes[47] & 4 && !(bytes[4] & 64)
    || bytes[4] & 128 && (!(bytes[4] & 48) || !bytes[46])
    || bytes[47] & 1 && (!(bytes[47] & 2) || !(bytes[4] & 32))) {
    throw new Error("Invalid wireless status report");
  }
  const ssid = bytes.subarray(10, 10 + bytes[9]);
  return {
    ...(settings ? { wifi_enabled: Boolean(settings.radios & 1), ble_enabled: Boolean(settings.radios & 2),
      tcp_cli_enabled: settings.tcp_cli_enabled } : {}),
    wifi_state: ["disabled", "connecting", "connected", "failed", "testing", "test_joined", "test_failed"][bytes[1]],
    state: bytes[1], detail: bytes[2] <= 6 ? bytes[2] : 0,
    phase: WIRELESS_TEST_DETAILS[bytes[2]] ?? "", ble_state: ["off", "advertising", "connected"][bytes[3]],
    network_configured: Boolean(bytes[4] & 1), ble_key_configured: Boolean(bytes[4] & 2),
    tcp_cli_started: Boolean(bytes[4] & 4), tcp_cli_auth_required: true,
    staged_mask: bytes[4] >> 4 & 7, pair_tested: Boolean(bytes[4] & 128), staged_state: bytes[47] & 15,
    ssid_staged: Boolean(bytes[4] & 16), password_staged: Boolean(bytes[4] & 32), key_staged: Boolean(bytes[4] & 64),
    error: bytes[2] <= 6 ? bytes[2] : 0,
    ipv4: [...bytes.subarray(5, 9)].join("."), ssid: displaySsid(ssid), ssid_hex: hex(ssid),
    scan_state: bytes[42], scan_generation: bytes[43] | bytes[44] << 8, scan_count: bytes[45],
    test_generation: bytes[46],
  };
}

export function decodeScanEntry(result) {
  const data = result.data;
  const raw = data.subarray(4, 4 + data[3]);
  const security = data[37];
  return { generation: data[0] | data[1] << 8, index: data[2], ssid: displaySsid(raw), ssid_hex: hex(raw),
    rssi: new DataView(data.buffer, data.byteOffset).getInt8(36), secured: Boolean(security),
    security: security & 4 ? security & 2 ? "WPA/RSN" : "RSN (WPA2/WPA3)"
      : security & 2 ? "WPA" : security & 1 ? "WEP" : "Open" };
}

export async function readWireless(client, device = client.device) {
  const generation = client.generation;
  const wireless = decodeWireless(await withTimeout(device.receiveFeatureReport(12), 1500), client.state.settings);
  if (device !== client.device || generation !== client.generation) throw new Error("Wireless connection changed");
  client.wirelessReceivedAt = client.now();
  client.wirelessInterval = wireless.state === 4 || wireless.scan_state === 1 ? 500 : 2000;
  if (!wireless.staged_mask && ![4, 5, 6].includes(wireless.state)) client.savedSsidHex = wireless.ssid_hex;
  client.receipts.set(12, client.now());
  client.scheduleExpiry();
  client.update({ wireless, wireless_error: null, freshness: { ...client.state.freshness, 12: "fresh" } });
  return wireless;
}

async function workflow(client, action) {
  if (!client.wirelessReady() || !(client.state.hardware?.capabilities & 128)) {
    throw new Error("Wireless configuration requires fresh idle USB storage, with sharing disabled.");
  }
  const token = Symbol("wireless operation");
  client.wirelessOperation = token;
  try { return await action(token); }
  finally { client.wirelessOperation = null; }
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

async function stagePair(client, token, settings, previous) {
  const ssid = ssidBytes(settings);
  const same = hex(ssid) === client.savedSsidHex;
  if (ssid.length && settings.password === undefined && !same) {
    throw new Error("A changed SSID requires an explicit password (empty for an open network)");
  }
  // Validate both fields before any write.
  const password = settings.password === undefined ? null : passwordBytes(settings.password);
  try {
    await stage(client, token, 6, ssid);
    if (password && ssid.length !== 0) await stage(client, token, 7, password);
  } finally { ssid.fill(0); password?.fill(0); }
}

function lifecycleOutcome(status, pending) {
  if (pending.operation === 11) {
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

async function waitLifecycle(client, pending, onProgress) {
  const device = client.device;
  const deadline = client.now() + (pending.operation === 9 ? 35000 : 15000);
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
    client.clearPersistedOperation();
    client.pendingWirelessCommand = null;
    if (outcome.kind === "failed") throw new Error(outcome.message);
    return outcome.result;
  } catch (error) {
    if (client.pendingWirelessCommand) client.update({ outcome_uncertain: true, error: error.message });
    throw error;
  }
}

export function registerWirelessRecovery(client) {
  client.reconcileWirelessCommand = async () => waitLifecycle(client, client.pendingWirelessCommand);
}

export async function scanWireless(client) {
  return workflow(client, async token => {
    const previous = await readWireless(client);
    const pending = { operation: 11, generation: previous.scan_generation % 65535 + 1 };
    await client.command(11, { workflow: token, lifecycle: pending });
    const status = await finishLifecycle(client, pending);
    const networks = [];
    for (let index = 0; index < status.scan_count; index++) {
      const result = await client.command(12, { workflow: token,
        args: [index, status.scan_generation & 255, status.scan_generation >> 8] });
      const entry = decodeScanEntry(result);
      networks.push(entry);
    }
    return networks;
  });
}

export async function testWireless(client, settings, onProgress) {
  return workflow(client, async token => {
    const previous = await readWireless(client);
    if (!ssidBytes(settings).length) throw new Error("SSID is required for a connection test");
    await stagePair(client, token, settings, previous);
    const pending = { operation: 9, generation: previous.test_generation % 255 + 1 };
    await client.command(9, { workflow: token, lifecycle: pending });
    const result = await finishLifecycle(client, pending, onProgress);
    client.update({ wireless_tested: true });
    return result;
  });
}

export async function cancelWireless(client) {
  return workflow(client, async token => {
    await client.command(10, { workflow: token });
    client.update({ wireless_tested: false });
    try { await readWireless(client); }
    catch (error) { client.update({ wireless: null, wireless_error: error.message }); }
  });
}

export async function saveWireless(client, settings) {
  return workflow(client, async token => {
    const previous = await readWireless(client);
    const board = settings.board;
    if (typeof settings.wifi_enabled !== "boolean" || typeof settings.ble_enabled !== "boolean"
      || settings.tcp_cli_enabled !== undefined && typeof settings.tcp_cli_enabled !== "boolean"
      || settings.use_tested_wifi !== undefined && typeof settings.use_tested_wifi !== "boolean"
      || settings.tcp_cli_auth_required !== undefined || !board
      || ![0, 150, 200, 250].includes(board.system_clock_mhz) || ![0, 1].includes(board.sdcard_profile)
      || board.radios !== (Number(settings.wifi_enabled) | Number(settings.ble_enabled) << 1)) {
      throw new Error("Invalid wireless configuration");
    }
    const tcp = settings.tcp_cli_enabled ?? client.state.settings.tcp_cli_enabled;
    const args = [board.system_clock_mhz & 255, board.system_clock_mhz >> 8, board.sdcard_profile,
      board.radios | Number(tcp) << 2];
    buildControlRequest(1, 5, { args }); // Validate board settings before staging.
    const target = ssidBytes(settings);
    const targetHex = hex(target);
    const targetLabel = displaySsid(target);
    let key;
    if (settings.ble_key !== undefined) {
      if (settings.ble_key !== "" && !/^[a-f0-9]{64}$/i.test(settings.ble_key)) {
        throw new Error("Control key must be 64 hex digits or empty to clear");
      }
      key = Uint8Array.from(settings.ble_key.match(/../g) ?? [], byte => parseInt(byte, 16));
    }
    if (settings.wifi_enabled && !target.length) throw new Error("Wi-Fi requires a network");
    if (settings.use_tested_wifi) {
      if (!previous.pair_tested || previous.staged_state & 8 || previous.ssid_hex !== hex(target)
        || settings.password !== undefined) throw new Error("Tested Wi-Fi pair no longer matches; test again");
    } else if (hex(target) !== previous.ssid_hex || settings.password !== undefined) {
      await stagePair(client, token, settings, previous);
    }
    try {
      if (key) await stage(client, token, 8, key);
    } finally { key?.fill(0); target.fill(0); }
    const staged = await readWireless(client);
    const result = await client.command(5, { workflow: token, flags: staged.staged_mask ? 2 : 0,
      args, commitMask: staged.staged_mask });
    return { ...result.settings, wifi_enabled: settings.wifi_enabled, ble_enabled: settings.ble_enabled,
      ssid: targetLabel, ssid_hex: targetHex, tcp_cli_enabled: tcp,
      ble_key_configured: settings.ble_key === undefined ? previous.ble_key_configured : Boolean(settings.ble_key) };
  });
}
