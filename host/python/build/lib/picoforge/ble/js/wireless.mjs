import { withTimeout } from "./device.mjs?v=4";

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

export function buildWireless(transaction, settings) {
  if (!Number.isInteger(transaction) || transaction < 1 || transaction > 255 ||
    typeof settings.wifi_enabled !== "boolean" || typeof settings.ble_enabled !== "boolean") {
    throw new Error("Invalid wireless configuration");
  }
  const ssid = new TextEncoder().encode(settings.ssid);
  if (typeof settings.ssid !== "string" || ssid.length > 32 || settings.ssid.includes("\0") ||
    (settings.wifi_enabled && !ssid.length)) throw new Error("Wi-Fi SSID must contain 1-32 UTF-8 bytes.");
  const report = new Uint8Array(136);
  report.set([1, transaction, Number(settings.wifi_enabled) | Number(settings.ble_enabled) << 1,
    Number(settings.password === undefined) | Number(settings.ble_key === undefined) << 1]);
  report.set(ssid, 4);
  if (settings.password !== undefined) {
    if (typeof settings.password !== "string" || !/^[\x20-\x7e]{8,63}$/.test(settings.password)) {
      throw new Error("Wi-Fi password must contain 8-63 printable ASCII characters.");
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
        client.update({ reboot_pending: true });
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
