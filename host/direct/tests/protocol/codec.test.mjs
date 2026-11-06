import assert from "node:assert/strict";
import test from "node:test";
import {
  decodeIdentityReport, decodeConfigReport, decodeTelemetryReport, decodeStorageReport, decodeSelfTestReport, decodeControlResult, buildControlRequest, decodeWireless, displaySsid, ssidBytes,
} from "../../web/js/protocol/codec.mjs";
import { identity, config, telemetry, storage, selfTest, result } from "../support/fixtures.mjs";
import { readFileSync } from "node:fs";
import { status, connected } from "../support/wireless.mjs";

const vectors = JSON.parse(readFileSync(new URL("../../../../tests/fixtures/wireless-status.json", import.meta.url)));

test("identity validates exact WebHID payload, product, layout, capabilities, board, and RAM", () => {
  const report = identity();
  const decoded = decodeIdentityReport(report);
  assert.equal(decoded.serial_number, "0102030405060708");
  assert.equal(decoded.firmware_version, "1.2.3");
  assert.equal(decoded.board_name, "Pico 2");
  for (const bytes of [report.slice(0, 25), Uint8Array.from([1, ...report]), new Uint8Array(42)]) {
    assert.throws(() => decodeIdentityReport(bytes));
  }
  for (const [offset, value] of [[0, 2], [1, 1], [17, 0], [17, 24], [14, 1]]) {
    const bad = report.slice(); bad[offset] = value;
    assert.throws(() => decodeIdentityReport(bad), `${offset}: ${value}`);
  }
  assert.equal(decodeIdentityReport(identity(0x90)).capabilities, 0x90);
  const reserved = report.slice(); reserved[16] = 255;
  assert.equal(decodeIdentityReport(reserved).product_name, "PicoForge", "identity reserved byte is ignored");
  const unknown = report.slice(); unknown[14] = 99; unknown[15] = 99;
  assert.equal(decodeIdentityReport(unknown).mcu, null);
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
  const radios = report.slice(); radios[13] |= 8;
  assert.throws(() => decodeConfigReport(radios, id));
  assert.equal(decodeConfigReport(radios, decodeIdentityReport(identity(1 | 2 | 4 | 16 | 32 | 128))).radios, 1);
  const unknownReset = config(); unknownReset[14] = 255;
  assert.equal(decodeConfigReport(unknownReset, id).reset_cause, 255);
});


test("configuration accepts non-GPIO LEDs and preserves GPIO validation", () => {
  const report = config();
  report[13] &= ~2;
  report[15] = 255;
  for (const [mcu, board] of [[1, 4], [2, 3]]) {
    const raw = identity(1 | 2 | 4 | 16 | 32 | 128);
    raw[14] = mcu;
    raw[15] = board;
    assert.equal(decodeConfigReport(report, decodeIdentityReport(raw)).led_gpio, 255);
  }
  const noLed = decodeIdentityReport(identity(2 | 4 | 16 | 32));
  assert.equal(decodeConfigReport(report, noLed).led_gpio, 255);
  assert.throws(() => decodeConfigReport(config(), noLed));
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
  const failed = selfTest(7); failed[28] = 7; failed[29] = 1;
  assert.equal(decodeSelfTestReport(failed).error, 7);
  failed[29] = 0;
  assert.throws(() => decodeSelfTestReport(failed));
  const unknown = selfTest(99);
  assert.equal(decodeSelfTestReport(unknown).state, null);
  assert.equal(decodeSelfTestReport(unknown).valid_for_control, false);
  const unknownError = selfTest(7); unknownError[28] = 255;
  assert.equal(decodeSelfTestReport(unknownError).error, 255);
  const rejected = passed.slice(); rejected[30] = 11;
  assert.throws(() => decodeSelfTestReport(rejected));
  const queued = selfTest(1); queued[8] = 1;
  assert.throws(() => decodeSelfTestReport(queued));
});


test("control request builders encode exact 68-byte envelopes and reject malformed operations", () => {
  const padded = prefix => [...prefix, ...Array(68 - prefix.length).fill(0)];
  assert.deepEqual([...buildControlRequest(1, 1, { args: [1] })], padded([1, 1, 1, 0, 1]));
  assert.deepEqual([...buildControlRequest(255, 3, { flags: 1, args: [2] })], padded([1, 255, 3, 1, 2]));
  assert.deepEqual([...buildControlRequest(2, 5, { args: [200, 0, 1] })], padded([1, 2, 5, 0, 200, 0, 1]));
  assert.deepEqual([...buildControlRequest(2, 5, { flags: 2, args: [200, 0, 1, 7] })],
    padded([1, 2, 5, 2, 200, 0, 1, 7]));
  for (const [opcode, options] of [[2, {}], [1, { flags: 1 }], [1, { args: [0, 1] }],
    [3, { args: [2] }], [3, { flags: 1, args: [1] }], [4, { args: [0, 1] }],
    [4, { args: [1, 0, 0, 0, 0] }], [5, { args: [100, 0, 0] }],
    [5, { args: [200, 0, 0, 0, 1] }], [5, { flags: 4, args: [200, 0, 0, 0] }], [6, { args: [33] }],
    [0, {}], [14, {}], [4, { args: [1, 1, 4, 0, 0] }], [10, { flags: 1 }], [11, { args: [1] }]]) {
    assert.throws(() => buildControlRequest(1, opcode, options), `opcode ${opcode}`);
  }
  assert.throws(() => buildControlRequest(0, 1));
  assert.equal(buildControlRequest(1, 4, { args: [1, 0, 4, 0, 0] })[6], 4, "1024 MiB is the self-test ceiling");
});


test("control results enforce every opcode/status/error combination and zero result data", () => {
  const allowed = {
    1: [[1, 0], [2, 0], [3, 1], [3, 3], [6, 6]],
    2: [[1, 0], [2, 0], [3, 1], [3, 2], [5, 5], [6, 6]],
    3: [[1, 0], [2, 0], [3, 1], [3, 2], [3, 3], [5, 5], [6, 6]],
    4: [[1, 0], [2, 0], [3, 1], [3, 3], [4, 4], [5, 5], [6, 6]],
    5: [[1, 0], [2, 0], [3, 1], [3, 3], [4, 4], [5, 5], [6, 6]],
    6: [[2, 0], [3, 1], [3, 3], [4, 4], [5, 5], [6, 6]],
    0: [[4, 7]], 255: [[4, 7]],
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
  const saved = result(1, 5); saved.set([200, 0, 1, 0], 5);
  assert.deepEqual(decodeControlResult(saved).settings,
    { system_clock_mhz: 200, sdcard_profile: 1, radios: 0, tcp_cli_enabled: false });
  saved[8] = 3;
  assert.equal(decodeControlResult(saved).settings.radios, 3);
  saved[8] = 6;
  assert.deepEqual(decodeControlResult(saved).settings,
    { system_clock_mhz: 200, sdcard_profile: 1, radios: 2, tcp_cli_enabled: true });
  saved[8] = 0;
  assert.equal(decodeControlResult(saved).settings.tcp_cli_enabled, false);
  saved[8] = 8;
  assert.throws(() => decodeControlResult(saved));
  saved[8] = 16;
  assert.throws(() => decodeControlResult(saved));
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


test("Report12 uses exact48byte vectors, raw SSIDs and no fabricated removed diagnostics", () => {
  const wireless = decodeWireless(Buffer.from(vectors.tcp_cli_auth_required, "hex"),
    { radios: 3, tcp_cli_enabled: true });
  assert.equal(wireless.ssid, "PicoForge");
  assert.equal(wireless.ssid_hex, "5069636f466f726765");
  assert.equal(wireless.tcp_cli_started, true);
  assert.equal(wireless.wifi_enabled, true);
  for (const field of ["rssi", "reconnect_count", "wifi_auth_state", "restart_required", "password_configured",
    "transaction_id", "tcp_cli_auth_required", "error"]) assert.equal(Object.hasOwn(wireless, field), false, field);
  const candidate = decodeWireless(Buffer.from(vectors.tested_candidate, "hex"));
  assert.equal(candidate.pair_tested, true);
  assert.equal(candidate.test_generation, 1);
  const lost = decodeWireless(Buffer.from(vectors.test_failed_saved_link_serving, "hex"));
  assert.deepEqual([lost.wifi_state, lost.ipv4, lost.pair_tested, lost.tcp_cli_started],
    ["test_failed", "42.42.42.42", true, true]);
  const init = decodeWireless(Buffer.from(vectors.radio_init_failed_while_connected, "hex"));
  assert.deepEqual([init.wifi_state, init.detail], ["connected", 1]);
  assert.equal(candidate.wifi_state, "test_joined");
  for (const bad of [new Uint8Array(53), status().slice(1), new Uint8Array(49)]) {
    assert.throws(() => decodeWireless(bad));
  }
  for (const [offset, value] of [[0, 2], [1, 7], [3, 3], [9, 33], [10, 1], [42, 4], [45, 19], [47, 1]]) {
    const bad = status(); bad[offset] = value;
    assert.throws(() => decodeWireless(bad), `${offset}/${value}`);
  }
  const reserved = status(); reserved[2] = 255; reserved[4] = 8; reserved[47] = 128;
  assert.equal(decodeWireless(reserved).detail, 0, "reserved detail/flag bits are ignored");
});


test("opaque SSIDs escape invalid UTF8 and controls but round-trip raw bytes rather than the label", () => {
  const raw = Uint8Array.from([0x61, 0, 255, 0xc3, 0xa9, 0x3c, 10]);
  const wireless = decodeWireless(status({ ssid: raw }));
  assert.equal(wireless.ssid, "a\\x00\\xFFé<\\x0A");
  assert.equal(wireless.ssid_hex, "6100ffc3a93c0a");
  assert.deepEqual(ssidBytes({ ssid: wireless.ssid, ssid_hex: wireless.ssid_hex.toUpperCase() }), raw);
  assert.equal(displaySsid(Uint8Array.from([0xe2, 0x80, 0xae])), "\\xE2\\x80\\xAE");
  assert.throws(() => ssidBytes({ ssid_hex: "bad" }));
  assert.throws(() => ssidBytes({ ssid_hex: "aa".repeat(33) }));
  assert.throws(() => ssidBytes({ ssid: "é".repeat(17) }));
});


test("staging, scan index/generation and proof requests use64argument bytes with zero padding", () => {
  for (const [op, args] of [[6, [3, 0, 255, 1]], [7, [8, ...new TextEncoder().encode("password")]],
    [8, [32, ...Array(32).fill(0xab)]], [9, []], [10, []], [11, []], [12, [17, 0x34, 0x12]],
    [13, Array(16).fill(123)]]) {
    const request = buildControlRequest(255, op, { args });
    assert.equal(request.length, 68);
    assert.deepEqual([...request.slice(0, 4 + args.length)], [1, 255, op, 0, ...args]);
    assert.ok(request.subarray(4 + args.length).every(byte => !byte));
  }
  for (const [op, args] of [[6, [33]], [7, [7, ...Array(7).fill(65)]], [7, [8, ...Array(8).fill(0)]],
    [8, [31]], [9, [1]], [12, [18, 1]], [12, [0, 0, 0]], [13, [...Array(16).fill(1), 1]]]) {
    assert.throws(() => buildControlRequest(1, op, { args }));
  }
});


test("strict result validation enforces details, scan padding, commit masks and opcode status rules", () => {
  for (const [op, state, error, detail] of [[6, 3, 3, 16], [7, 3, 3, 17], [8, 3, 3, 18],
    [9, 3, 3, 19], [5, 3, 3, 20], [9, 3, 3, 21], [12, 3, 3, 22], [12, 3, 3, 23],
    [13, 3, 3, 24], [8, 5, 5, 25], [11, 5, 5, 26], [5, 3, 3, 27], [5, 5, 5, 28]]) {
    const bytes = result(1, op, state, error); bytes[5] = detail;
    assert.equal(decodeControlResult(bytes, { ble: detail === 28 }).detail, detail);
    if (detail === 28) assert.throws(() => decodeControlResult(bytes));
    bytes[6] = 1; assert.throws(() => decodeControlResult(bytes));
    bytes[6] = 0; bytes[2] = 1; assert.throws(() => decodeControlResult(bytes));
  }
  for (const op of [6, 7, 8, 10, 12, 13]) assert.throws(() => decodeControlResult(result(1, op, 1)));
  const scan = result(1, 12); scan.set([1, 0, 0, 1, 255], 5); scan[41] = 216; scan[42] = 7;
  assert.doesNotThrow(() => decodeControlResult(scan));
  for (const [offset, value] of [[5, 0], [7, 18], [8, 33], [10, 1], [42, 8], [43, 1]]) {
    const bad = scan.slice(); bad[offset] = value; assert.throws(() => decodeControlResult(bad));
  }
  const saved = result(1, 5); saved.set([200, 0, 1, 7, 7], 5);
  assert.equal(decodeControlResult(saved).commit_mask, 7);
  saved[9] = 8; assert.throws(() => decodeControlResult(saved));
});

