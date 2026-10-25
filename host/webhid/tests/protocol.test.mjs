import assert from "node:assert/strict";
import test from "node:test";
import {
  decodeIdentityReport, decodeConfigReport, decodeTelemetryReport, decodeStorageReport, decodeSelfTestReport,
  decodeControlResult, buildControlRequest, matchesPicoForge,
} from "../js/device.mjs";
import { identity, config, telemetry, storage, selfTest, result, fakeConnection, emit } from "./support/fixtures.mjs";

test("identity validates exact WebHID payload, product, layout, capabilities, board, and RAM", () => {
  const report = identity();
  const decoded = decodeIdentityReport(report);
  assert.equal(decoded.serial_number, "0102030405060708");
  assert.equal(decoded.firmware_version, "1.2.3");
  assert.equal(decoded.board_name, "Pico 2");
  for (const bytes of [report.slice(0, 25), Uint8Array.from([1, ...report]), new Uint8Array(42)]) {
    assert.throws(() => decodeIdentityReport(bytes));
  }
  for (const [offset, value] of [[0, 2], [1, 1], [17, 0], [17, 24], [17, 0x90], [14, 1]]) {
    const bad = report.slice(); bad[offset] = value;
    assert.throws(() => decodeIdentityReport(bad), `${offset}: ${value}`);
  }
  const reserved = report.slice(); reserved[16] = 255;
  assert.equal(decodeIdentityReport(reserved).product_name, "PicoForge", "identity reserved byte is ignored");
  const unknown = report.slice(); unknown[14] = 99; unknown[15] = 99;
  assert.equal(decodeIdentityReport(unknown).mcu, "Unknown MCU (99)");
  assert.equal(decodeIdentityReport(unknown).board_model, 99);
  for (const [total, used] of [[0, 0], [100, 101]]) {
    const bad = report.slice(); const view = new DataView(bad.buffer);
    view.setUint32(18, total, true); view.setUint32(22, used, true);
    assert.throws(() => decodeIdentityReport(bad));
  }
});

test("configuration validates profiles, selected-transport GPIOs, flags and LED capability", () => {
  const id = decodeIdentityReport(identity());
  const report = config();
  assert.equal(decodeConfigReport(report, id).sdcard_profile, 1);
  for (const [offset, value] of [[1, 100], [3, 2], [4, 2], [5, 255], [7, 255],
    [7, 30], [13, 128], [15, 255], [16, 255]]) {
    const bad = report.slice(); bad[offset] = value;
    assert.throws(() => decodeConfigReport(bad, id), `${offset}: ${value}`);
  }
  const spi = decodeIdentityReport(identity(8 | 1));
  const spiConfig = config("spi");
  assert.equal(decodeConfigReport(spiConfig, spi).storage_gpios[4], 255);
  spiConfig[11] = 10;
  assert.throws(() => decodeConfigReport(spiConfig, spi));
  const unknownReset = config(); unknownReset[14] = 255;
  assert.equal(decodeConfigReport(unknownReset, id).reset_cause, 255);
});

test("telemetry preserves 64-bit uptime, load sentinels, temperature and exact length", () => {
  const report = telemetry();
  const view = new DataView(report.buffer);
  view.setBigUint64(4, 2n ** 63n + 123n, true);
  view.setUint16(12, 65535, true);
  const decoded = decodeTelemetryReport(report);
  assert.equal(decoded.uptime_ms, 2n ** 63n + 123n);
  assert.equal(decoded.cpu_usage_core0_basis_points, null);
  assert.equal(decoded.temperature_centidegrees, -1234);
  for (const [offset, value] of [[1, 2], [21, 128], [21, 0]]) {
    const bad = report.slice(); bad[offset] = value;
    assert.throws(() => decodeTelemetryReport(bad));
  }
  for (const load of [10001, 65534]) {
    view.setUint16(12, load, true);
    assert.throws(() => decodeTelemetryReport(report));
  }
  assert.throws(() => decodeTelemetryReport(Uint8Array.from([...telemetry(), 0])));
  const absent = telemetry(); absent[1] = 0; absent[21] = 3;
  new DataView(absent.buffer).setInt16(2, 0, true);
  assert.equal(decodeTelemetryReport(absent).temperature_centidegrees, null);
});

test("storage validates independent SD sample, ownership, timing, capacity and CID", () => {
  const id = decodeIdentityReport(identity());
  const decoded = decodeStorageReport(storage(), id);
  assert.equal(decoded.memory.capacity_bytes, 512 * 100000);
  assert.equal(decoded.memory.manufacturer.product_name, "SD16G");
  assert.equal(decoded.memory.manufacturer.serial_number, 0x12345678);
  assert.equal(decoded.memory.manufacturer.manufacture_year, 2025);
  for (const [offset, value] of [[2, 128], [2, 63], [2, 64], [2, 71], [4, 2], [9, 0], [9, 3],
    [31, 1], [32, 1], [33, 2], [30, 5]]) {
    const bad = storage(); bad[offset] = value;
    assert.throws(() => decodeStorageReport(bad, id), `${offset}: ${value}`);
  }
  const unavailable = new Uint8Array(34);
  unavailable[0] = 1; unavailable[2] = 1; unavailable[31] = 1;
  assert.equal(decodeStorageReport(unavailable, id).memory.sample_valid, false);
  assert.equal(decodeStorageReport(unavailable, id).msc.enabled, true);
  unavailable[14] = 1;
  assert.throws(() => decodeStorageReport(unavailable, id));
  const absent = new Uint8Array(34); absent[0] = 1; absent[2] = 67; absent[3] = 1; absent[4] = 1;
  assert.equal(decodeStorageReport(absent, id).memory.present, false);
  absent[2] |= 16;
  assert.throws(() => decodeStorageReport(absent, id));
  assert.throws(() => decodeStorageReport(storage().slice(0, 33), id));
});

test("self-test validates retained states, execution/request errors and unknown diagnostics", () => {
  const idle = selfTest();
  assert.equal(decodeSelfTestReport(idle).state, "idle");
  const passed = selfTest(6);
  assert.equal(decodeSelfTestReport(passed).progress_percent, 100);
  for (const [offset, value] of [[0, 2], [3, 4], [28, 3], [28, 8], [29, 1], [30, 3], [30, 9], [31, 1]]) {
    const bad = idle.slice(); bad[offset] = value;
    assert.throws(() => decodeSelfTestReport(bad), `${offset}: ${value}`);
  }
  const failed = selfTest(7); failed[28] = 8; failed[29] = 1;
  assert.equal(decodeSelfTestReport(failed).error, 8);
  failed[29] = 0;
  assert.throws(() => decodeSelfTestReport(failed));
  const unknown = selfTest(99);
  assert.equal(decodeSelfTestReport(unknown).state, "Unknown state (99)");
  assert.equal(decodeSelfTestReport(unknown).valid_for_control, false);
  const unknownError = selfTest(7); unknownError[28] = 255;
  assert.equal(decodeSelfTestReport(unknownError).error, 255);
  const rejected = passed.slice(); rejected[30] = 11;
  assert.equal(decodeSelfTestReport(rejected).state, "passed");
  const queued = selfTest(1); queued[8] = 1;
  assert.throws(() => decodeSelfTestReport(queued));
});

test("control request builders encode new nine-byte envelopes and reject malformed operations", () => {
  assert.deepEqual([...buildControlRequest(1, 1, { args: [1] })], [1, 1, 1, 0, 1, 0, 0, 0, 0]);
  assert.deepEqual([...buildControlRequest(255, 3, { flags: 1, args: [2] })], [1, 255, 3, 1, 2, 0, 0, 0, 0]);
  assert.deepEqual([...buildControlRequest(2, 6, { args: [200, 0, 1] })], [1, 2, 6, 0, 200, 0, 1, 0, 0]);
  for (const [opcode, options] of [[2, {}], [1, { flags: 1 }], [1, { args: [0, 1] }],
    [3, { args: [2] }], [3, { flags: 1, args: [1] }], [4, { args: [0, 1] }],
    [4, { args: [1, 0, 0, 0, 0] }], [6, { args: [100, 0, 0] }], [5, {}]]) {
    assert.throws(() => buildControlRequest(1, opcode, options), `opcode ${opcode}`);
  }
  assert.throws(() => buildControlRequest(0, 1));
});

test("control results enforce every opcode/status/error combination and zero result data", () => {
  const allowed = {
    1: [[1, 0], [2, 0], [3, 1], [3, 3], [4, 4], [6, 6]],
    2: [[1, 0], [2, 0], [3, 1], [3, 2], [4, 4], [5, 5], [6, 6]],
    3: [[1, 0], [2, 0], [3, 1], [3, 2], [3, 3], [5, 5], [6, 6]],
    4: [[1, 0], [2, 0], [3, 1], [3, 3], [4, 4], [5, 5], [6, 6]],
    6: [[1, 0], [2, 0], [3, 1], [3, 3], [4, 4], [5, 5], [6, 6]],
    5: [[4, 7]], 0: [[4, 7]], 255: [[4, 7]],
  };
  for (const [op, combinations] of Object.entries(allowed)) {
    for (let status = 0; status <= 7; status++) for (let error = 0; error <= 8; error++) {
      const bytes = result(1, Number(op), status, error);
      const valid = combinations.some(pair => pair[0] === status && pair[1] === error);
      if (valid) assert.doesNotThrow(() => decodeControlResult(bytes), `${op}/${status}/${error}`);
      else assert.throws(() => decodeControlResult(bytes), `${op}/${status}/${error}`);
    }
  }
  assert.equal(decodeControlResult(result(0, 0, 0)).status, 0);
  for (const report of [result(0, 1), Uint8Array.from([...result(1, 1), 0])]) {
    assert.throws(() => decodeControlResult(report));
  }
  const badData = result(1, 1); badData[5] = 1;
  assert.throws(() => decodeControlResult(badData));
  const saved = result(1, 6); saved.set([200, 0, 1, 0], 5);
  assert.deepEqual(decodeControlResult(saved).settings, { system_clock_mhz: 200, sdcard_profile: 1 });
  saved[8] = 1;
  assert.throws(() => decodeControlResult(saved));
});

test("mandatory connection reads reject legacy firmware/config failures and gate self-test input", async () => {
  const { client, device, reads } = fakeConnection({ capabilities: 1 | 2 | 4 | 16 });
  await client.connect();
  assert.deepEqual(reads, [1, 2]);
  assert.equal(client.state.connected, true);
  emit(device, 11, selfTest());
  assert.equal(client.state.self_test, null);
  await client.disconnect();
  const old = fakeConnection();
  old.device.receiveFeatureReport = async () => new DataView(new Uint8Array(42).buffer);
  await old.client.connect();
  assert.equal(old.client.state.connected, false);
  assert.match(old.client.state.error, /exactly 26/);
  const missing = fakeConnection();
  missing.device.receiveFeatureReport = async id => {
    if (id === 1) return identity();
    throw new Error("missing config");
  };
  await missing.client.connect();
  assert.equal(missing.client.state.connected, false);
  assert.match(missing.client.state.error, /missing config/);
});

test("freshness is independent, exact at deadlines, and clears caches on disconnect/reconnect", async () => {
  let now = 0;
  const { client, device, hid } = fakeConnection({ now: () => now });
  await client.connect();
  assert.equal(client.state.freshness[3], "unavailable");
  emit(device, 3, telemetry()); emit(device, 10, storage()); emit(device, 11, selfTest());
  now = 749; client.expireReports();
  assert.ok(client.state.runtime);
  now = 750; client.expireReports();
  assert.equal(client.state.runtime, null);
  assert.equal(client.state.freshness[3], "stale");
  assert.ok(client.state.memory);
  now = 1499; client.expireReports();
  assert.ok(client.state.self_test);
  now = 1500; client.expireReports();
  assert.equal(client.state.memory, null);
  assert.equal(client.state.self_test, null);
  emit(device, 3, telemetry());
  assert.equal(client.state.freshness[3], "fresh");
  const invalid = telemetry(); invalid[0] = 2;
  now = 2000; emit(device, 3, invalid);
  now = 2250; client.expireReports();
  assert.equal(client.state.runtime, null, "invalid reports never refresh receipt time");
  emit(device, 3, telemetry()); client.expireReports(true);
  assert.equal(client.state.runtime, null);
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  for (const field of ["hardware", "settings", "runtime", "board_status", "memory", "msc", "self_test"]) {
    assert.equal(client.state[field], null, field);
  }
  await client.attachDevice(device);
  assert.equal(client.state.freshness[10], "unavailable");
  assert.equal(client.state.memory, null);
  await client.disconnect();
});

test("transactions serialize, match ID and opcode, poll Pending, wrap IDs, and do not mutate telemetry", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect(); emit(device, 3, telemetry());
  let calls = 0;
  const original = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id !== 20) return original(id);
    calls++;
    const [transaction, opcode] = sent.at(-1).slice(2, 4);
    if (calls === 1) return result(transaction, 3);
    if (calls === 2) return result(255, opcode);
    return result(transaction, opcode, calls === 3 ? 1 : 2);
  };
  client.transaction = 255;
  const pending = client.setLed(true);
  await assert.rejects(client.setLed(true), /unavailable/);
  await pending;
  assert.deepEqual(sent[0], [20, 1, 1, 1, 0, 1, 0, 0, 0, 0]);
  assert.equal(calls, 4);
  assert.equal(client.state.board_status.led_on, false, "acceptance is not an Input snapshot");
  await client.disconnect();
});

test("SET failure never polls; terminal rejection permits a new transaction; timeout blocks replay", async () => {
  const { client, device, reads } = fakeConnection();
  await client.connect(); emit(device, 3, telemetry());
  device.sendFeatureReport = async () => { throw new Error("SET rejected"); };
  await assert.rejects(client.setLed(true), /SET rejected/);
  assert.deepEqual(reads, [1, 2]);
  assert.equal(client.state.outcome_uncertain, false);
  let request;
  device.sendFeatureReport = async (id, payload) => { request = payload; };
  device.receiveFeatureReport = async () => result(request[1], request[2], 6, 6);
  await assert.rejects(client.setLed(true), /status 6, error 6/);
  assert.equal(client.state.outcome_uncertain, false);
  device.receiveFeatureReport = async () => result(request[1], request[2]);
  await assert.doesNotReject(client.setLed(true));
  const timeout = fakeConnection({ sleep: async () => {}, timeout: 10000 });
  await timeout.client.connect(); emit(timeout.device, 3, telemetry());
  timeout.device.receiveFeatureReport = async () => result(0, 0, 0);
  await assert.rejects(timeout.client.setLed(true), /uncertain/);
  await assert.rejects(timeout.client.setLed(true), /uncertain/);
  assert.equal(timeout.sent.length, 1);
  await timeout.client.disconnect();
  await client.disconnect();
});

test("all decoders reject padded, short and unsupported-layout payloads", () => {
  const id = decodeIdentityReport(identity());
  for (const [decode, bytes] of [[decodeIdentityReport, identity()],
    [data => decodeConfigReport(data, id), config()], [decodeTelemetryReport, telemetry()],
    [data => decodeStorageReport(data, id), storage()], [decodeSelfTestReport, selfTest()],
    [decodeControlResult, result(1, 1)]]) {
    assert.throws(() => decode(bytes.slice(0, -1)));
    assert.throws(() => decode(Uint8Array.from([...bytes, 0])));
    const wrongVersion = bytes.slice(); wrongVersion[0] = 2;
    assert.throws(() => decode(wrongVersion));
    const buffer = new Uint8Array(bytes.length + 2); buffer.set(bytes, 1);
    assert.doesNotThrow(() => decode(new DataView(buffer.buffer, 1, bytes.length)));
  }
});

test("stale/capability gates stop command writes, including self-test and confirmed reboot", async () => {
  let now = 0;
  const { client, device, sent } = fakeConnection({ now: () => now, capabilities: 16 | 1 });
  await client.connect();
  emit(device, 3, telemetry()); emit(device, 10, storage());
  await assert.rejects(client.reboot(true), /capability/);
  assert.equal(await client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 }), false);
  await assert.rejects(client.setSelfTest("start", 32), /self-test/);
  now = 1500;
  await assert.rejects(client.setLed(true), /Fresh LED/);
  await assert.rejects(client.setMsc(true), /Fresh idle/);
  assert.equal(sent.length, 0);
  await client.disconnect();
});

test("hanging SET/GET requests time out as uncertain and never replay", async () => {
  for (const operation of ["sendFeatureReport", "receiveFeatureReport"]) {
    const { client, device, sent } = fakeConnection({ timeout: 10 });
    await client.connect(); emit(device, 3, telemetry());
    device[operation] = () => new Promise(() => {});
    await assert.rejects(client.setLed(true), /timeout/);
    assert.equal(client.state.outcome_uncertain, true);
    await assert.rejects(client.setLed(true), /uncertain/);
    assert.equal(sent.length, operation === "sendFeatureReport" ? 0 : 1);
    await client.disconnect();
  }
});

test("disconnect during GET clears caches and reconnect resolves only the original result, without SET replay", async () => {
  const { client, device, hid, sent } = fakeConnection();
  await client.connect();
  emit(device, 3, telemetry());
  let release;
  const normalRead = device.receiveFeatureReport;
  device.receiveFeatureReport = id => id === 20 ? new Promise(resolve => { release = resolve; }) : normalRead(id);
  const command = client.setLed(true);
  await new Promise(resolve => setImmediate(resolve));
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  assert.equal(client.state.hardware, null);
  assert.equal(client.state.outcome_uncertain, true);
  device.receiveFeatureReport = normalRead;
  await client.attachDevice(device);
  assert.equal(client.state.connected, true);
  assert.equal(client.state.runtime, null);
  assert.equal(client.state.outcome_uncertain, false);
  release(result(sent[0][2], 1));
  await assert.rejects(command, /disconnect/);
  assert.equal(client.state.error, null, "old request must not overwrite the reconnected state");
  assert.equal(sent.length, 1);
  await client.disconnect();
});

test("MSC-style re-enumeration reconnects by Feature 1 UID, not a non-standard serialNumber property", async () => {
  const { client, device, hid } = fakeConnection();
  assert.equal("serialNumber" in device, false);
  await client.connect();
  const uid = client.state.hardware.serial_number;
  hid.dispatchEvent(Object.assign(new Event("disconnect"), { device }));
  device.opened = false;
  assert.equal(client.state.reconnecting, true);

  const receive = device.receiveFeatureReport;
  const otherIdentity = identity();
  otherIdentity[3] ^= 0xff;
  device.receiveFeatureReport = id => id === 1 ? otherIdentity : receive(id);
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, false, "another PicoForge device must not be attached");
  assert.equal(client.state.reconnecting, true);

  device.receiveFeatureReport = receive;
  device.opened = false;
  hid.dispatchEvent(Object.assign(new Event("connect"), { device }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.connected, true);
  assert.equal(client.state.hardware.serial_number, uid);
  await client.disconnect();
});

test("settings readback remains serialized, verifies both result and config, and reports failure", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect();
  emit(device, 3, telemetry()); emit(device, 10, storage()); emit(device, 11, selfTest());
  const normalRead = device.receiveFeatureReport;
  let release;
  device.receiveFeatureReport = id => id === 2 ? new Promise(resolve => { release = resolve; }) : normalRead(id);
  const saving = client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(client.state.busy, true);
  await assert.rejects(client.setLed(true), /unavailable/);
  const incorrect = config(); incorrect[1] = 150;
  release(incorrect);
  assert.equal(await saving, false);
  assert.match(client.state.settings_message, /did not confirm/);
  assert.equal(sent.length, 1);
  assert.equal(client.state.settings.system_clock_mhz, 200);
  await client.disconnect();
});

test("MSC/self-test acceptance waits for fresh asynchronous status and settings save never reboots", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  assert.equal(await client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 }), true);
  assert.deepEqual(sent[0], [20, 1, 1, 6, 0, 200, 0, 1, 0, 0]);
  assert.equal(sent.length, 1, "save does not schedule reboot");
  await assert.rejects(client.reboot(false), /confirmation/);
  await client.reboot(true);
  assert.deepEqual(sent[1].slice(0, 5), [20, 1, 2, 2, 1]);
  await assert.rejects(client.setSelfTest("start", 32), /unavailable/);
  await client.disconnect();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  await client.setSelfTest("start", 32);
  assert.deepEqual(sent[2], [20, 1, 3, 4, 0, 1, 32, 0, 0, 0]);
  assert.equal(client.state.self_test.state, "idle", "acceptance doesn't invent progress");
  await assert.rejects(client.setMsc(true), /Fresh idle/);
  emit(device, 11, selfTest(6));
  await client.setMsc(true);
  assert.equal(client.state.msc.enabled, false, "acceptance doesn't invent ownership");
  assert.equal(client.state.msc_command_pending, true);
  const host = new Uint8Array(34); host[0] = 1; host[2] = 1; host[31] = 1;
  emit(device, 10, host);
  await assert.rejects(client.reboot(true), /firmware-owned/);
  await client.setMsc(false, true);
  assert.deepEqual(sent.at(-1).slice(0, 6), [20, 1, 5, 3, 1, 2]);
  await client.disconnect();
});

test("development HID filter uses authorized vendor collection and both USB configurations", () => {
  const { device } = fakeConnection();
  assert.equal(matchesPicoForge(device), true);
  device.productId = 0x4012; assert.equal(matchesPicoForge(device), true);
  device.collections[0].usage = 2; assert.equal(matchesPicoForge(device), false);
});
