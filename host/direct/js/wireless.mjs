import { decodeConfigReport, withTimeout } from "./device.mjs?v=8";

export function decodeWireless(report) {
  const bytes = report instanceof DataView
    ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;
  const restartFlag = bytes instanceof Uint8Array && bytes.length === 53 ? bytes[52] : 0;
  if (!(bytes instanceof Uint8Array) || ![52, 53].includes(bytes.length)
    || ![1, 2, 3, 4].includes(bytes[0]) || bytes.length !== (bytes[0] >= 4 ? 53 : 52) || bytes[1] > 3
    || bytes[2] > 6 || bytes[4] > 3 || bytes[42] > 1 || bytes[43] > 1 || bytes[44] > 3
    || bytes[45] > 2 || bytes[46] > 1
    || bytes[47] > (bytes[0] === 1 ? 0 : bytes[0] === 2 ? 1 : 3)
    || bytes[0] < 3 && bytes.subarray(48, 52).some(Boolean) || bytes[0] >= 3 && bytes[48] > 3
    || restartFlag > 1
    || bytes.slice(9, 42).indexOf(0) < 0) {
    throw new Error("Invalid wireless status report");
  }
  const ssid = bytes.slice(9, 42);
  return { wifi_enabled: Boolean(bytes[1] & 1), ble_enabled: Boolean(bytes[1] & 2),
    status: bytes[2], transaction_id: bytes[3], wifi_state: ["disabled", "connecting", "connected", "failed"][bytes[4]],
    ipv4: [...bytes.slice(5, 9)].join("."), ssid: new TextDecoder("utf-8", { fatal: true }).decode(ssid.slice(0, ssid.indexOf(0))),
    password_configured: Boolean(bytes[42]), ble_key_configured: Boolean(bytes[43]), error: bytes[44],
    ble_state: ["off", "advertising", "connected"][bytes[45]], tcp_cli_started: Boolean(bytes[46]),
    tcp_cli_enabled: !(bytes[47] & 1), tcp_cli_configurable: bytes[0] >= 2,
    tcp_cli_auth_required: Boolean(bytes[47] & 2),
    wifi_auth_state: bytes[0] < 3 ? null : ["try_wpa2", "try_sae", "wpa2", "sae"][bytes[48]],
    reconnect_count: bytes[0] < 3 ? 0 : new DataView(bytes.buffer, bytes.byteOffset).getUint16(49, true),
    rssi: bytes[0] < 3 || bytes[51] === 127 ? null : new DataView(bytes.buffer, bytes.byteOffset).getInt8(51),
    restart_required: bytes[0] >= 4 && Boolean(restartFlag) };
}

export function buildWirelessCommand(transaction, operation, settings = {}) {
  if (!Number.isInteger(transaction) || transaction < 1 || transaction > 255
    || ![1, 2, 3, 4].includes(operation)) throw new Error("Invalid wireless command");
  const report = new Uint8Array(101);
  report[0] = 1;
  report[1] = operation;
  report[2] = transaction;
  if (operation === 2) {
    if (!Number.isInteger(settings.page) || settings.page < 0 || settings.page > 2) {
      throw new Error("Invalid wireless scan page");
    }
    report[3] = settings.page;
  } else if (operation === 3) {
    if (typeof settings.ssid !== "string" || !settings.ssid || settings.ssid.includes("\0")) {
      throw new Error("SSID is required for a connection test");
    }
    const ssid = new TextEncoder().encode(settings.ssid);
    if (ssid.length > 32) throw new Error("SSID must be at most 32 UTF-8 bytes");
    const password = settings.password ?? "";
    if (typeof password !== "string" || password.length !== 0
      && !/^[\x20-\x7e]{8,63}$/.test(password)) {
      throw new Error("Password must contain 8-63 printable ASCII characters");
    }
    const secret = new TextEncoder().encode(password);
    report[4] = ssid.length;
    report[5] = secret.length;
    report.set(ssid, 6);
    report.set(secret, 38);
    secret.fill(0);
  }
  return report;
}

/** Feature 35 byte 3 while testing (states 4..6): join phase or failure reason. */
export const WIRELESS_TEST_DETAILS = ["", "Searching for network", "Checking password", "Waiting for IP address",
  "Network not found", "Password rejected", "Connected but no IP address from DHCP", "Timed out",
  "Could not start connection", "Test connection lost", "Test credentials expired"];

export function decodeWirelessResult(report) {
  const bytes = report instanceof DataView
    ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;
  if (!(bytes instanceof Uint8Array) || bytes.length !== 222 || ![1, 2].includes(bytes[0]) || bytes[2] > 6
    || bytes[3] > (bytes[2] >= 4 ? bytes[0] === 1 ? 8 : WIRELESS_TEST_DETAILS.length - 1 : 2)
    || bytes[4] > 18 || bytes[5] > 3) {
    throw new Error("Invalid wireless scan/test result");
  }
  const results = [];
  for (let i = 0; i < 6; i++) {
    const entry = bytes.subarray(6 + i * 35, 41 + i * 35);
    const length = entry[0];
    if (length === 0) {
      if (entry.some(Boolean)) throw new Error("Invalid empty wireless scan result");
      continue;
    }
    if (length > 32 || entry[34] > (bytes[0] === 1 ? 1 : 7) || entry.subarray(1 + length, 33).some(Boolean)) {
      throw new Error("Invalid wireless scan entry");
    }
    results.push({ ssid: new TextDecoder("utf-8", { fatal: true }).decode(entry.subarray(1, 1 + length)),
      rssi: new DataView(entry.buffer, entry.byteOffset).getInt8(33), secured: Boolean(entry[34]),
      security: bytes[0] === 1 ? entry[34] ? "Secured" : "Open"
        : ["Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)", "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN"][entry[34]] });
  }
  const remainingSeconds = new DataView(bytes.buffer, bytes.byteOffset).getUint16(220, true);
  const staged = bytes[2] === 5 || bytes[2] === 6 && bytes[3] === 9;
  if (results.length > bytes[4] || bytes.subarray(216, 220).some(value => bytes[2] !== 5 && value !== 0)
    || remainingSeconds > 600 || !staged && remainingSeconds !== 0) {
    throw new Error("Invalid wireless scan/test result fields");
  }
  const detail = bytes[2] >= 4 ? bytes[3] : 0;
  return { transaction_id: bytes[1], state: bytes[2], page: bytes[2] >= 4 ? 0 : bytes[3], detail,
    phase: WIRELESS_TEST_DETAILS[detail], count: bytes[4], wifi_state: bytes[5], networks: results,
    ipv4: [...bytes.subarray(216, 220)].join("."),
    remaining_seconds: staged ? remainingSeconds : 0 };
}

export function buildWireless(transaction, settings, tcpCliConfigurable = true) {
  if (settings.tcp_cli_enabled !== undefined
    && (typeof settings.tcp_cli_enabled !== "boolean" || !tcpCliConfigurable)) {
    throw new Error("TCP CLI setting requires a boolean and wireless status v2");
  }
  if (settings.tcp_cli_auth_required !== undefined) {
    throw new Error("TCP CLI authentication follows the saved control key and cannot be set");
  }
  if (!Number.isInteger(transaction) || transaction < 1 || transaction > 255 ||
    typeof settings.wifi_enabled !== "boolean" || typeof settings.ble_enabled !== "boolean") {
    throw new Error("Invalid wireless configuration");
  }
  const board = settings.board;
  if (![0, 150, 200, 250].includes(board?.system_clock_mhz) || ![0, 1].includes(board.sdcard_profile)
    || ![0, 1, 2, 3].includes(board.radios)
    || settings.wifi_enabled !== Boolean(board.radios & 1)
    || settings.ble_enabled !== Boolean(board.radios & 2)) throw new Error("Invalid board configuration");
  const ssid = new TextEncoder().encode(settings.ssid);
  if (typeof settings.ssid !== "string" || ssid.length > 32 || settings.ssid.includes("\0") ||
    (settings.wifi_enabled && !ssid.length)) throw new Error("Wi-Fi SSID must contain 1-32 UTF-8 bytes.");
  const report = new Uint8Array(136);
  report.set([2, transaction, board.radios,
    Number(settings.password === undefined && !settings.use_tested_wifi)
      | Number(settings.ble_key === undefined) << 1 | Number(Boolean(settings.use_tested_wifi)) << 2
      | (tcpCliConfigurable ? settings.tcp_cli_enabled === undefined ? 16 : settings.tcp_cli_enabled ? 0 : 8 : 0)]);
  report.set([board.system_clock_mhz & 255, board.system_clock_mhz >> 8, board.sdcard_profile], 133);
  report.set(ssid, 4);
  if (settings.password !== undefined) {
    if (typeof settings.password !== "string" || settings.password.length !== 0
      && !/^[\x20-\x7e]{8,63}$/.test(settings.password)) {
      throw new Error("Wi-Fi password must be empty or contain 8-63 printable ASCII characters.");
    }
    report.set(new TextEncoder().encode(settings.password), 37);
  }
  // An empty ble_key erases the saved key; the report field stays zero.
  if (settings.ble_key) {
    if (!/^[a-f0-9]{64}$/i.test(settings.ble_key) || /^0+$/.test(settings.ble_key)) {
      throw new Error("BLE key must be 64 nonzero hex digits.");
    }
    report.set(Uint8Array.from(settings.ble_key.match(/../g), byte => parseInt(byte, 16)), 101);
  }
  return report;
}

async function wirelessCommand(client, operation, settings = {}, terminalStates = [2], onProgress = null) {
  if (!client.wirelessReady() || !(client.state.hardware?.capabilities & 128)) {
    throw new Error("Wireless scan and connection test require fresh idle USB storage, with sharing disabled.");
  }
  const transaction = client.transaction % 255 + 1;
  const report = buildWirelessCommand(transaction, operation, settings);
  client.transaction = transaction;
  const device = client.device;
  const generation = client.generation;
  const pending = { uid: client.state.hardware.serial_number, transaction, operation, terminalStates,
    timeout: operation === 3 ? 35000 : 15000 };
  client.reconcileWirelessCommand = (target, command) => reconcileWirelessCommand(target, command);
  client.update({ busy: true, error: null, recovery_message: null });
  let persisted = false;
  try {
    client.pendingWirelessCommand = pending;
    client.persistPendingOperation();
    persisted = true;
    await withTimeout(device.sendFeatureReport(34, report), 5000);
    const deadline = Date.now() + pending.timeout;
    while (Date.now() < deadline) {
      if (device !== client.device || generation !== client.generation) {
        throw new Error("Wireless request interrupted by disconnect.");
      }
      const result = decodeWirelessResult(await withTimeout(device.receiveFeatureReport(35), 1500));
      if (result.transaction_id === transaction) {
        const outcome = wirelessCommandOutcome(result, pending);
        if (outcome) {
          client.clearPersistedOperation();
          pending.outcome = outcome;
          client.pendingWirelessCommand = null;
          if (outcome.kind === "failed") throw new Error(outcome.message);
          return outcome.result;
        }
        if (result.state === 4) onProgress?.(result.phase);
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error("Wireless operation timed out; do not retry until its result is checked.");
  } catch (error) {
    if (pending.outcome?.kind === "completed") return pending.outcome.result;
    if (pending.outcome?.kind === "failed") throw new Error(pending.outcome.message);
    if (!persisted && generation === client.generation) {
      client.pendingWirelessCommand = null;
      client.update({ outcome_uncertain: false, error: error.message });
      throw error;
    }
    if (generation === client.generation) {
      client.update({ outcome_uncertain: true, error: error.message });
    }
    throw error;
  } finally {
    report.fill(0);
    if (generation === client.generation) client.update({ busy: false });
  }
}

function wirelessCommandOutcome(result, pending) {
  if (result.state === 6) return { kind: "failed", message: pending.operation === 3
    ? `Could not connect: ${result.phase || "Timed out"}.` : "Wireless operation failed." };
  return pending.terminalStates.includes(result.state) ? { kind: "completed", result } : null;
}

export async function reconcileWirelessCommand(device, pending) {
  const deadline = Date.now() + pending.timeout;
  while (Date.now() < deadline) {
    const result = decodeWirelessResult(await withTimeout(device.receiveFeatureReport(35), 1500));
    if (result.transaction_id === pending.transaction) {
      const outcome = wirelessCommandOutcome(result, pending);
      if (outcome) return outcome;
    }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  return null;
}

export function registerWirelessRecovery(client) {
  client.reconcileWirelessCommand = (device, pending) => reconcileWirelessCommand(device, pending);
  client.reconcileWirelessSave = (device, pending) => reconcileWirelessSave(client, device, pending);
}

export async function scanWireless(client) {
  const result = await wirelessCommand(client, 1, {}, [2]);
  const pages = [];
  for (let page = 0; page < Math.ceil(result.count / 6); page++) {
    const selected = await wirelessCommand(client, 2, { page }, [2]);
    pages.push(...selected.networks);
  }
  return pages;
}

export async function testWireless(client, settings, onProgress) {
  const result = await wirelessCommand(client, 3, settings, [5], onProgress);
  client.update({ wireless_tested: true });
  return result;
}

export async function cancelWireless(client) {
  await wirelessCommand(client, 4, {}, [0]);
  client.update({ wireless_tested: false });
}

async function digestText(value) {
  const bytes = new TextEncoder().encode(value);
  try {
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
    return [...digest].map(byte => byte.toString(16).padStart(2, "0")).join("");
  } finally { bytes.fill(0); }
}

async function wirelessSaveMatches(wireless, expected) {
  return wireless.tcp_cli_enabled === expected.tcp_cli_enabled
    && wireless.wifi_enabled === expected.wifi_enabled
    && wireless.ble_enabled === expected.ble_enabled
    && await digestText(wireless.ssid) === expected.ssid_digest
    && wireless.password_configured === expected.password_configured
    && wireless.ble_key_configured === expected.ble_key_configured;
}

function boardSaveMatches(board, expected) {
  return board.system_clock_mhz === expected.system_clock_mhz
    && board.sdcard_profile === expected.sdcard_profile && board.radios === expected.radios;
}

export async function reconcileWirelessSave(client, device, pending) {
  const wireless = decodeWireless(await withTimeout(device.receiveFeatureReport(30), 1000));
  if (wireless.transaction_id !== pending.transaction || wireless.status <= 1) return null;
  if (wireless.status !== 2) {
    return { kind: "rejected", message: `Wireless configuration rejected: status ${wireless.status}` };
  }
  if (!await wirelessSaveMatches(wireless, pending.expected.wireless)) return null;
  const board = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), 1000), client.state.hardware);
  if (!boardSaveMatches(board, pending.expected.board)) return null;
  return { kind: "saved", wireless, board };
}

export async function saveWireless(client, settings) {
  if (!client.wirelessReady() || !(client.state.hardware?.capabilities & 128)) {
    throw new Error("Wireless configuration requires fresh idle USB storage, with sharing disabled.");
  }
  const transaction = client.transaction % 255 + 1;
  let report;
  client.transaction = transaction;
  const device = client.device;
  const generation = client.generation;
  let pending = null;
  client.reconcileWirelessSave = (target, operation) => reconcileWirelessSave(client, target, operation);
  client.update({ busy: true, error: null, recovery_message: null });
  let persisted = false;
  try {
    const previous = decodeWireless(await withTimeout(device.receiveFeatureReport(30), 1000));
    if (device !== client.device || generation !== client.generation) throw new Error("Wireless save interrupted by disconnect.");
    report = buildWireless(transaction, settings, previous.tcp_cli_configurable);
    pending = { uid: client.state.hardware.serial_number, transaction,
      expected: { wireless: {
        tcp_cli_enabled: settings.tcp_cli_enabled ?? previous.tcp_cli_enabled,
        wifi_enabled: settings.wifi_enabled, ble_enabled: settings.ble_enabled,
        ssid_digest: await digestText(settings.ssid),
        password_configured: settings.use_tested_wifi
          || (settings.password !== undefined ? Boolean(settings.password) : previous.password_configured),
        ble_key_configured: settings.ble_key !== undefined ? Boolean(settings.ble_key)
          : previous.ble_key_configured,
      }, board: { system_clock_mhz: settings.board.system_clock_mhz,
        sdcard_profile: settings.board.sdcard_profile, radios: settings.board.radios } } };
    client.pendingWirelessSave = pending;
    client.persistPendingOperation();
    persisted = true;
    await withTimeout(device.sendFeatureReport(31, report), 5000);
    if (device !== client.device || generation !== client.generation) throw new Error("Wireless save interrupted by disconnect.");
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const saved = decodeWireless(await withTimeout(device.receiveFeatureReport(30), 1000));
      if (device !== client.device || generation !== client.generation) throw new Error("Wireless save interrupted by disconnect.");
      if (saved.transaction_id === transaction && saved.status > 1) {
        if (saved.status !== 2) {
          client.clearPersistedOperation();
          pending.outcome = { kind: "rejected",
            message: `Wireless configuration rejected: status ${saved.status}` };
          client.pendingWirelessSave = null;
          client.update({ outcome_uncertain: false });
          throw new Error(`Wireless configuration rejected: status ${saved.status}`);
        }
        if (!await wirelessSaveMatches(saved, pending.expected.wireless)) {
          throw new Error("Wireless save did not confirm the requested configuration.");
        }
        const board = decodeConfigReport(await withTimeout(device.receiveFeatureReport(2), 1000),
          client.state.hardware);
        if (device !== client.device || generation !== client.generation) throw new Error("Wireless save interrupted by disconnect.");
        if (!boardSaveMatches(board, pending.expected.board)) {
          throw new Error("Board configuration save was not confirmed.");
        }
        client.clearPersistedOperation();
        pending.outcome = { kind: "saved", wireless: saved, board };
        client.pendingWirelessSave = null;
        client.update({ settings: board, wireless: saved, wireless_error: null, reboot_pending: true,
          outcome_uncertain: false, wireless_tested: false });
        return saved;
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error("Wireless save result timed out; inspect after reconnect, never replay.");
  } catch (error) {
    if (pending?.outcome?.kind === "saved") return pending.outcome.wireless;
    if (pending?.outcome?.kind === "rejected") throw new Error(pending.outcome.message);
    if (generation === client.generation) {
      if (!persisted) client.pendingWirelessSave = null;
      client.update({ outcome_uncertain: Boolean(pending && client.pendingWirelessSave === pending),
        error: error.message });
    }
    throw error;
  } finally {
    report?.fill(0);
    if (generation === client.generation) client.update({ busy: false });
  }
}
