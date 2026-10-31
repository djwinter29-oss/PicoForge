import { decodeConfigReport, withTimeout } from "./device.mjs?v=4";

export function decodeWireless(report) {
  const bytes = report instanceof DataView
    ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;
  if (!(bytes instanceof Uint8Array) || bytes.length !== 48 || bytes[0] !== 1 || bytes[1] > 3
    || bytes[2] > 6 || bytes[4] > 3 || bytes[42] > 1 || bytes[43] > 1 || bytes[44] > 3
    || bytes.slice(45).some(Boolean) || bytes.slice(9, 42).indexOf(0) < 0) {
    throw new Error("Invalid wireless status report");
  }
  const ssid = bytes.slice(9, 42);
  return { wifi_enabled: Boolean(bytes[1] & 1), ble_enabled: Boolean(bytes[1] & 2),
    status: bytes[2], transaction_id: bytes[3], wifi_state: ["disabled", "connecting", "connected", "failed"][bytes[4]],
    ipv4: [...bytes.slice(5, 9)].join("."), ssid: new TextDecoder("utf-8", { fatal: true }).decode(ssid.slice(0, ssid.indexOf(0))),
    password_configured: Boolean(bytes[42]), ble_key_configured: Boolean(bytes[43]), error: bytes[44] };
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
  "Network not found", "Password rejected", "Connected but no IP address from DHCP", "Timed out"];

export function decodeWirelessResult(report) {
  const bytes = report instanceof DataView
    ? new Uint8Array(report.buffer, report.byteOffset, report.byteLength) : report;
  if (!(bytes instanceof Uint8Array) || bytes.length !== 220 || bytes[0] !== 1 || bytes[2] > 6
    || bytes[3] > (bytes[2] >= 4 ? 7 : 2) || bytes[4] > 18 || bytes[5] > 3) {
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
    if (length > 32 || entry[34] > 1 || entry.subarray(1 + length, 33).some(Boolean)) {
      throw new Error("Invalid wireless scan entry");
    }
    results.push({ ssid: new TextDecoder("utf-8", { fatal: true }).decode(entry.subarray(1, 1 + length)),
      rssi: new DataView(entry.buffer, entry.byteOffset).getInt8(33), secured: Boolean(entry[34]) });
  }
  if (results.length > bytes[4] || bytes.subarray(216).some((value, i) => bytes[2] !== 5 && value !== 0 && i < 4)) {
    throw new Error("Invalid wireless scan/test result fields");
  }
  const detail = bytes[2] >= 4 ? bytes[3] : 0;
  return { transaction_id: bytes[1], state: bytes[2], page: detail ? 0 : bytes[3], detail,
    phase: WIRELESS_TEST_DETAILS[detail], count: bytes[4], wifi_state: bytes[5], networks: results, ipv4: [...bytes.subarray(216, 220)].join(".") };
}

export function buildWireless(transaction, settings) {
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
      | Number(settings.ble_key === undefined) << 1 | Number(Boolean(settings.use_tested_wifi)) << 2]);
  report.set([board.system_clock_mhz & 255, board.system_clock_mhz >> 8, board.sdcard_profile], 133);
  report.set(ssid, 4);
  if (settings.password !== undefined) {
    if (typeof settings.password !== "string" || settings.password.length !== 0
      && !/^[\x20-\x7e]{8,63}$/.test(settings.password)) {
      throw new Error("Wi-Fi password must be empty or contain 8-63 printable ASCII characters.");
    }
    report.set(new TextEncoder().encode(settings.password), 37);
  }
  if (settings.ble_key !== undefined) {
    if (!/^[a-f0-9]{64}$/i.test(settings.ble_key) || /^0+$/.test(settings.ble_key)) {
      throw new Error("BLE key must be 64 nonzero hex digits.");
    }
    report.set(Uint8Array.from(settings.ble_key.match(/../g), byte => parseInt(byte, 16)), 101);
  }
  return report;
}

async function wirelessCommand(client, operation, settings = {}, terminalStates = [2], onProgress = null) {
  if (client.state.transport === "ble" || !(client.state.hardware?.capabilities & 128) ||
    !client.storageReady() || client.state.msc.enabled || client.state.self_test?.active ||
    client.state.busy || client.state.outcome_uncertain || client.state.reboot_pending || client.outstanding) {
    throw new Error("Wireless scan and connection test require fresh idle USB storage, with sharing disabled.");
  }
  const transaction = client.transaction % 255 + 1;
  const report = buildWirelessCommand(transaction, operation, settings);
  client.transaction = transaction;
  const device = client.device;
  const generation = client.generation;
  client.update({ busy: true, error: null });
  let sent = false;
  let resultKnown = false;
  try {
    await withTimeout(device.sendFeatureReport(34, report), 5000);
    sent = true;
    const deadline = Date.now() + (operation === 3 ? 35000 : 15000);
    while (Date.now() < deadline) {
      if (device !== client.device || generation !== client.generation) {
        throw new Error("Wireless request interrupted by disconnect.");
      }
      const result = decodeWirelessResult(await withTimeout(device.receiveFeatureReport(35), 1500));
      if (result.transaction_id === transaction) {
        if (result.state === 6) {
          resultKnown = true;
          throw new Error(operation === 3
            ? `Could not connect: ${result.phase || "Timed out"}.` : "Wireless operation failed.");
        }
        if (terminalStates.includes(result.state)) {
          resultKnown = true;
          return result;
        }
        if (result.state === 4) onProgress?.(result.phase);
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error("Wireless operation timed out; do not retry until its result is checked.");
  } catch (error) {
    if (sent && !resultKnown) {
      client.update({ outcome_uncertain: true });
    }
    throw error;
  } finally {
    report.fill(0);
    client.update({ busy: false });
  }
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

export async function saveWireless(client, settings) {
  if (client.state.transport === "ble" || !(client.state.hardware?.capabilities & 128) ||
    !client.storageReady() || client.state.msc.enabled || client.state.self_test?.active ||
    client.state.busy || client.state.outcome_uncertain || client.state.reboot_pending || client.outstanding) {
    throw new Error("Wireless configuration requires fresh idle USB storage, with sharing disabled.");
  }
  const transaction = client.transaction % 255 + 1;
  const report = buildWireless(transaction, settings);
  client.transaction = transaction;
  client.update({ busy: true, error: null });
  let attempted = false;
  try {
    const previous = decodeWireless(await withTimeout(client.device.receiveFeatureReport(30), 1000));
    attempted = true;
    await withTimeout(client.device.sendFeatureReport(31, report), 5000);
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const saved = decodeWireless(await withTimeout(client.device.receiveFeatureReport(30), 1000));
      if (saved.transaction_id === transaction && saved.status > 1) {
        if (saved.status !== 2) {
          attempted = false;
          throw new Error(`Wireless configuration rejected: status ${saved.status}`);
        }
        if (saved.wifi_enabled !== settings.wifi_enabled || saved.ble_enabled !== settings.ble_enabled ||
          saved.ssid !== settings.ssid ||
          saved.password_configured !== (settings.use_tested_wifi || settings.password !== undefined ||
            previous.password_configured) ||
          saved.ble_key_configured !== (settings.ble_key !== undefined || previous.ble_key_configured)) {
          throw new Error("Wireless save did not confirm the requested configuration.");
        }
        const board = decodeConfigReport(await withTimeout(client.device.receiveFeatureReport(2), 1000),
          client.state.hardware);
        if (board.system_clock_mhz !== settings.board.system_clock_mhz
          || board.sdcard_profile !== settings.board.sdcard_profile || board.radios !== settings.board.radios) {
          throw new Error("Board configuration save was not confirmed.");
        }
        client.update({ settings: board, reboot_pending: true, wireless_tested: false });
        return saved;
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error("Wireless save result timed out; inspect after reconnect, never replay.");
  } catch (error) {
    client.update({ outcome_uncertain: attempted, error: error.message });
    throw error;
  } finally { report.fill(0); client.update({ busy: false }); }
}
