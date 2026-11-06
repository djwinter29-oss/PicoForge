// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import {
  acknowledge, DashboardError, reconciledResult, responseJson, savedSettings, savedWireless, scannedBoards,
  scannedNetworks, statusResponse, testedWireless,
} from "../../src/js/dashboard-api.mjs";

test("mutation boundary distinguishes refusals, malformed replies, and unknown delivery", async () => {
  for (const body of [null, [], true, "ok"]) {
    await assert.rejects(responseJson({ ok: true, json: async () => body }, "save", true),
      error => error instanceof DashboardError && error.uncertain === true);
  }
  const invalid = { json: async () => { throw new SyntaxError("invalid JSON"); } };
  await assert.rejects(responseJson({ ok: true, ...invalid }, "save", true), { uncertain: true });
  await assert.rejects(responseJson({ ok: true, ...invalid }, "status"), { uncertain: false });
  for (const [status, body, uncertain] of [
    [409, { error: "busy" }, false], [404, { error: "unknown board" }, false],
    [503, { error: "not supported" }, false], [503, { error: "read failed" }, true],
    [503, { error: "refused", uncertain: false }, false], [409, { error: "pending", uncertain: true }, true],
  ]) {
    await assert.rejects(responseJson({ ok: false, status, json: async () => body }, "fallback", true),
      error => error.status === status && error.uncertain === uncertain);
  }
  await assert.rejects(responseJson({ ok: false, status: 409, ...invalid }, "known refusal", true),
    { status: 409, uncertain: false, message: "known refusal" });
});

test("status validates lifecycle and admission facts before allowing any transition", () => {
  const valid = { connected: true, reboot_pending: false, command_outcome_uncertain: false,
    settings_supported: true, telemetry_stale: false, storage_stale: false, self_test_stale: false,
    runtime: { led_available: true, led_on: false }, hardware: { capabilities: 128 },
    memory: { initialized: true, msc: { enabled: false, transition_pending: false } },
    board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 1 },
    self_test: { active: false, sequence: 255 },
    wireless: { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true, ble_enabled: false,
      ble_key_configured: false, tcp_cli_enabled: true } };
  assert.equal(statusResponse(valid), valid);
  const disconnected = { ...valid, connected: false, settings_supported: false,
    hardware: null, runtime: null, memory: null, board_configuration: null, self_test: null, wireless: null };
  assert.equal(statusResponse(disconnected), disconnected);
  assert.equal(statusResponse({ ...valid, diagnostics: { future_field: "accepted" } }).connected, true);
  assert.throws(() => statusResponse({}), /Invalid status/);
  for (const field of ["connected", "reboot_pending", "command_outcome_uncertain", "settings_supported",
    "telemetry_stale", "storage_stale", "self_test_stale"]) {
    for (const invalid of [undefined, null, 0, 1, "false", [], {}])
      assert.throws(() => statusResponse({ ...valid, [field]: invalid }), /Invalid status/);
  }
  for (const field of ["reconnecting", "command_resync_supported", "wireless_cleanup_required", "outcome_uncertain"])
    assert.throws(() => statusResponse({ ...valid, [field]: "false" }), /Invalid status/);
  for (const field of ["hardware", "runtime", "memory", "self_test", "board_configuration", "wireless"])
    for (const invalid of [[], false, "status"])
      assert.throws(() => statusResponse({ ...valid, [field]: invalid }), /Invalid status/);
  for (const [field, section] of [
    ["hardware", { capabilities: "32" }], ["runtime", { led_on: "false" }],
    ["memory", { msc: { enabled: false, transition_pending: "false" } }],
    ["self_test", { active: false }], ["self_test", { active: "false", sequence: 1 }],
    ["self_test", { active: false, sequence: 256 }], ["board_configuration", {}],
    ["wireless", {}], ["wireless", { ...valid.wireless, ssid_hex: "f" }],
    ["wireless", { ...valid.wireless, tcp_cli_enabled: "false" }],
    ["wireless", { ...valid.wireless, test_generation: "1" }],
    ["wireless", { ...valid.wireless, key_staged: "false" }],
  ]) assert.throws(() => statusResponse({ ...valid, [field]: section }), /Invalid status/);
});

test("successful Wi-Fi testing requires a validated generation and firmware lifetime", () => {
  for (const test_generation of [1, 255])
    assert.deepEqual(testedWireless({ test_generation, remaining_seconds: 600, ipv4: "192.0.2.1",
      test_id: "a".repeat(32) }), { test_generation, remaining_seconds: 600, ipv4: "192.0.2.1",
      test_id: "a".repeat(32) });
  for (const test_generation of [undefined, null, 0, 256, "1", 1.5])
    assert.throws(() => testedWireless({ test_generation, remaining_seconds: 600, test_id: "a".repeat(32) }),
      { uncertain: true });
  for (const remaining_seconds of [undefined, null, 0, -1, 601, Infinity, "600", 1.5])
    assert.throws(() => testedWireless({ test_generation: 1, remaining_seconds, test_id: "a".repeat(32) }),
      { uncertain: true });
  for (const test_id of [undefined, null, {}, "", "a".repeat(31), "A".repeat(32), "g".repeat(32)])
    assert.throws(() => testedWireless({ test_generation: 1, remaining_seconds: 600, test_id }), { uncertain: true });
  for (const value of [null, [], {}, { test_generation: 1, remaining_seconds: 600,
    test_id: "a".repeat(32), ipv4: {} }])
    assert.throws(() => testedWireless(value), { uncertain: true });
});

test("malformed optional diagnostics are explicitly isolated without changing command admission facts", () => {
  const valid = { connected: true, reboot_pending: false, command_outcome_uncertain: false,
    settings_supported: true, telemetry_stale: false, storage_stale: false, self_test_stale: false,
    runtime: { led_available: true, led_on: false, system_clock_hz: 200000000 },
    hardware: { capabilities: 2 }, board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0 },
    memory: { initialized: true, msc: { enabled: false, transition_pending: false } } };
  for (const manufacturer of [null, {}, { id: 3 }, { serial_number: 0 }, { product_revision: "8.0" }]) {
    const result = statusResponse({ ...valid, memory: { ...valid.memory, manufacturer } });
    assert.equal(result.diagnostic_error, undefined, "missing optional manufacturer facts are supported");
  }
  const malformed = { ...valid, runtime: { ...valid.runtime, temperature_centidegrees: "hot" },
    memory: { ...valid.memory, manufacturer: { id: 3, serial_number: {}, product_name: "Card", product_revision: [] } },
    board_configuration: { ...valid.board_configuration, storage_gpios: {} } };
  const result = statusResponse(malformed);
  assert.deepEqual(result.memory.msc, valid.memory.msc);
  assert.equal(result.runtime.led_available, true);
  assert.equal(result.runtime.system_clock_hz, 200000000);
  assert.equal(result.memory.manufacturer.id, 3);
  assert.equal(result.memory.manufacturer.serial_number, undefined);
  assert.equal(result.board_configuration.storage_gpios, undefined);
  assert.match(result.diagnostic_error, /memory.manufacturer.serial_number/);
  assert.match(result.diagnostic_error, /runtime.temperature_centidegrees/);
  assert.deepEqual(malformed.memory.manufacturer.serial_number, {}, "validation must not mutate the source");
  for (const bad of [false, [], "manufacturer"]) {
    const result = statusResponse({ ...valid, memory: { ...valid.memory, manufacturer: bad } });
    assert.equal(result.memory.manufacturer, null);
    assert.match(result.diagnostic_error, /memory.manufacturer/);
  }
});

test("scan boundary validates each network without changing raw SSID bytes", () => {
  const network = { ssid: "\\xFF\\x00", ssid_hex: "ff00", rssi: -30, secured: false, security: "Open" };
  assert.deepEqual(scannedNetworks({ networks: [network] }), [network]);
  assert.deepEqual(scannedNetworks({ networks: [] }), []);
  for (const invalid of [null, [], "network", { ...network, ssid: null }, { ...network, ssid_hex: "f" },
    { ...network, ssid_hex: "gg" }, { ...network, ssid_hex: "ff".repeat(33) },
    { ...network, secured: "false" }, { ...network, rssi: NaN }, { ...network, rssi: -129 },
    { ...network, rssi: 128 }, { ...network, rssi: -30.5 }, { ...network, security: {} }]) {
    assert.throws(() => scannedNetworks({ networks: [network, invalid] }), { uncertain: true });
  }
  for (const networks of [undefined, null, {}, "networks"])
    assert.throws(() => scannedNetworks({ networks }), { uncertain: true });
});

test("optional diagnostic ranges match CID dates, integer GPIO bytes and UART indices", () => {
  const valid = { connected: true, reboot_pending: false, command_outcome_uncertain: false, settings_supported: true,
    telemetry_stale: false, storage_stale: false, self_test_stale: false,
    hardware: { capabilities: 2 }, runtime: { led_available: true, led_on: false },
    memory: { msc: { enabled: false, transition_pending: false } },
    board_configuration: { system_clock_mhz: 200, sdcard_profile: 0, radios: 0 } };
  for (const [field, accepted, rejected] of [
    ["id", [0, 255], [-1, 256, 1.5]],
    ["manufacturer_id", [0, 255], [-1, 256, 1.5]],
    ["manufacture_year", [2000, 2255], [1999, 2256, 2000.5]],
    ["manufacture_month", [1, 12], [0, 13, 1.5]],
    ["serial_number", [0, 0xffffffff], [-1, 0x100000000, 0.5]],
  ]) {
    for (const value of accepted) {
      const result = statusResponse({ ...valid, memory: { ...valid.memory, manufacturer: { [field]: value } } });
      assert.equal(result.memory.manufacturer[field], value);
      assert.equal(result.diagnostic_error, undefined);
    }
    for (const value of rejected) {
      const result = statusResponse({ ...valid, memory: { ...valid.memory, manufacturer: { [field]: value } } });
      assert.equal(result.memory.manufacturer[field], undefined);
      assert.ok(result.diagnostic_error.includes(`memory.manufacturer.${field}`));
    }
  }
  for (const [field, accepted, rejected] of [
    ["monitor_uart_index", [0, 1, 255], [-1, 2, 256, 0.5]],
    ["monitor_uart_tx_gpio", [0, 47, 255], [-1, 256, 0.5]],
    ["monitor_uart_rx_gpio", [0, 47, 255], [-1, 256, 0.5]],
    ["card_detect_gpio", [0, 47, 255], [-1, 256, 0.5]],
    ["reset_cause", [0, 4, 255], [-1, 256, 0.5]],
    ["storage_gpios", [[0, 1, 2, 3, 255, 255], [40, 41, 42, 43, 44, 45]], [[-1], [256], [0.5]]],
  ]) {
    for (const value of accepted) {
      const result = statusResponse({ ...valid, board_configuration: { ...valid.board_configuration, [field]: value } });
      assert.deepEqual(result.board_configuration[field], value);
      assert.equal(result.diagnostic_error, undefined);
    }
    for (const value of rejected) {
      const source = { ...valid, board_configuration: { ...valid.board_configuration, [field]: value } };
      const result = statusResponse(source);
      assert.equal(result.board_configuration[field], undefined);
      assert.ok(result.diagnostic_error.includes(`board_configuration.${field}`));
      assert.equal(result.runtime.led_available, true);
      assert.deepEqual(source.board_configuration[field], value);
    }
  }
});

test("mutation success contracts accept both hosts and reject contradictory results", () => {
  for (const opcode of [1, 2, 4]) {
    assert.deepEqual(acknowledge({}, opcode), { status: "completed", opcode });
    assert.deepEqual(acknowledge({ control_result: { status: "completed", opcode } }, opcode),
      { status: "completed", opcode });
    for (const result of [{ ok: false }, { control_result: { status: "pending", opcode } },
      { control_result: { status: "completed", opcode: 255 } }])
      assert.throws(() => acknowledge(result, opcode), { uncertain: true });
  }
  const settings = { system_clock_mhz: 200, sdcard_profile: 0, radios: 3 };
  assert.deepEqual(savedSettings({ settings }, settings), settings);
  for (const result of [{}, { settings: { ...settings, radios: "3" } },
    { settings: { ...settings, system_clock_mhz: 250 } }])
    assert.throws(() => savedSettings(result, settings), { uncertain: true });
});

test("combined save validates explicit credential intent without inventing omitted SSID ownership", () => {
  const wireless = { ssid: "saved", ssid_hex: "7361766564", wifi_enabled: true, ble_enabled: true,
    ble_key_configured: true, tcp_cli_enabled: true };
  const result = { wireless, reboot_pending: true };
  const request = { wifi_enabled: true, ble_enabled: true, tcp_cli_enabled: true, ble_key: "key" };
  assert.equal(savedWireless(result, request), wireless);
  assert.equal(savedWireless(result, { ...request, ssid: "saved" }), wireless);
  assert.equal(savedWireless(result, { ...request, ssid_hex: "7361766564" }), wireless);
  for (const changes of [{ wifi_enabled: false }, { ble_enabled: false }, { tcp_cli_enabled: false },
    { ble_key: "" }, { ssid: "" }, { ssid_hex: "ff" }])
    assert.throws(() => savedWireless(result, { ...request, ...changes }), { uncertain: true });
  for (const changes of [{ reboot_pending: false }, { wireless: null }, { wireless: {} }])
    assert.throws(() => savedWireless({ ...result, ...changes }, request), { uncertain: true });
});

test("board discovery rejects malformed records instead of silently publishing an empty success", () => {
  const board = { id: "board", device_path: "/hid/board", serial_number: "serial" };
  assert.deepEqual(scannedBoards({ boards: [board] }), [board]);
  assert.deepEqual(scannedBoards({ boards: [] }), []);
  for (const boards of [undefined, null, {}, [null], [{ ...board, id: "" }],
    [{ ...board, device_path: 1 }], [{ ...board, serial_number: 1 }]])
    assert.throws(() => scannedBoards({ boards }), /Invalid board discovery response/);
});

test("reconciliation accepts only terminal results or an explicitly clean host", () => {
  for (const control_result of [undefined, null])
    assert.equal(reconciledResult({ uncertain: false, control_result }), null);
  for (const status of ["completed", "invalid", "unsupported", "busy", "failed"]) {
    const control_result = { opcode: 4, status, error: "storage_busy", detail: 1 };
    assert.deepEqual(reconciledResult({ uncertain: false, control_result }), control_result);
  }
  for (const control_result of [{}, [], true, { opcode: 4, status: "pending" }, { opcode: 4, status: "idle" },
    { opcode: "4", status: "failed" }, { opcode: 0, status: "failed" }, { opcode: 14, status: "completed" },
    { opcode: 4.5, status: "failed" }, { opcode: 4, status: "failed", error: {} },
    { opcode: 4, status: "failed", detail: 256 }]) {
    assert.throws(() => reconciledResult({ uncertain: false, control_result }), { uncertain: true });
  }
  for (const uncertain of [undefined, null, true, "false"])
    assert.throws(() => reconciledResult({ uncertain }), { uncertain: true });
});
