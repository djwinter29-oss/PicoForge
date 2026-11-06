import assert from "node:assert/strict";
import test from "node:test";
import { mscPending } from "../../web/js/state/selectors.mjs";
import { telemetry, storage, selfTest, result, fakeConnection, emit, says } from "../support/fixtures.mjs";
import { Operations } from "../../web/js/board/operations.mjs";
import { BOARD, status, connected, radio } from "../support/wireless.mjs";

test("USB TCP CLI proof is board-bound and serialized through Feature 20 opcode 13", async () => {
  const { client, device, sent } = fakeConnection({ capabilities: 128 | 55 });
  const proof = Uint8Array.from({ length: 16 }, (_, index) => index + 16);
  const receive = device.receiveFeatureReport;
  device.receiveFeatureReport = async id => {
    if (id === 20) { const bytes = result(1, 13); bytes.set(proof, 5); return bytes; }
    return receive(id);
  };
  await client.connect();
  const nonce = Uint8Array.from({ length: 16 }, (_, index) => index);
  const uid = Uint8Array.from({ length: 8 }, (_, index) => index + 1);
  assert.deepEqual(await client.authorizeConsole(nonce, uid), proof);
  assert.deepEqual(sent[0], [20, 1, 1, 13, 0, ...nonce, ...Array(48).fill(0)]);
  await assert.rejects(client.authorizeConsole(nonce, Uint8Array.from([8, 7, 6, 5, 4, 3, 2, 1])),
    says(/different board/));
  assert.equal(sent.length, 1, "a mismatched challenge is rejected before contacting the board");
  await client.disconnect();
});


test("stale/capability gates stop command writes, including self-test and confirmed reboot", async () => {
  let now = 0;
  const { client, device, sent } = fakeConnection({ now: () => now, capabilities: 16 | 1 });
  await client.connect();
  emit(device, 3, telemetry()); emit(device, 10, storage());
  await assert.rejects(client.reboot(true), says(/capability/));
  await assert.rejects(client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 }), says(/capability/));
  await assert.rejects(client.setSelfTest("start", 32), says(/self-test/));
  now = 1500;
  await assert.rejects(client.setLed(true), says(/Fresh LED/));
  await assert.rejects(client.setMsc(true), says(/Fresh idle/));
  assert.equal(sent.length, 0);
  await client.disconnect();
});


test("W-board settings save sends and verifies the radio enables", async () => {
  const { client, device, sent } = fakeConnection({ capabilities: 1 | 2 | 4 | 16 | 32 | 128 });
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  await client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1, radios: 2 });
  assert.deepEqual(sent[0], [20, 1, 1, 5, 0, 200, 0, 1, 2, ...Array(60).fill(0)]);
  assert.equal(client.state.settings.radios, 2);
  await client.disconnect();
});


test("MSC/self-test acceptance waits for fresh status and settings save queues firmware reboot", async () => {
  const { client, device, sent } = fakeConnection();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  await client.saveSettings({ system_clock_mhz: 200, sdcard_profile: 1 });
  assert.deepEqual(sent[0], [20, 1, 1, 5, 0, 200, 0, 1, 0, ...Array(60).fill(0)]);
  assert.equal(sent.length, 1, "firmware reboot requires no second command");
  assert.equal(client.state.reboot_pending, true);
  await client.disconnect();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  await assert.rejects(client.reboot(false), says(/confirmation/));
  await client.reboot(true);
  assert.deepEqual(sent[1].slice(0, 5), [20, 1, 2, 2, 1]);
  await assert.rejects(client.setSelfTest("start", 32), says(/unavailable/));
  await client.disconnect();
  await client.connect();
  emit(device, 10, storage()); emit(device, 11, selfTest());
  await client.setSelfTest("start", 32);
  assert.deepEqual(sent[2], [20, 1, 3, 4, 0, 1, 32, 0, 0, 0, ...Array(59).fill(0)]);
  assert.equal(client.state.self_test.state, "idle", "acceptance doesn't invent progress");
  await assert.rejects(client.setMsc(true), says(/Fresh idle/));
  const completed = selfTest(6); completed[1] = 1;
  emit(device, 11, completed);
  await client.setMsc(true);
  assert.equal(client.state.msc.enabled, false, "acceptance doesn't invent ownership");
  assert.equal(mscPending(client.state), true);
  const host = new Uint8Array(34); host[0] = 1; host[2] = 1; host[31] = 1;
  emit(device, 10, host);
  await assert.rejects(client.reboot(true), says(/firmware-owned/));
  await client.setMsc(false, true);
  assert.deepEqual(sent.at(-1).slice(0, 6), [20, 1, 5, 3, 1, 2]);
  await client.disconnect();
});


test("configuration save selects the wireless workflow from the board link and intent", async () => {
  const calls = [];
  const session = {
    state: { link: "usb" },
    async saveWireless(intent) { calls.push(["wireless", intent]); return { ssid: intent.ssid }; },
    async saveSettings(...args) { calls.push(["settings", ...args]); },
  };
  const operations = new Operations(session);
  const settings = { radios: 3 };
  const wireless = { ssid: "PicoForge" };

  assert.deepEqual(await operations.saveConfiguration({ settings, force: true, wireless }),
    { ssid: "PicoForge" });
  assert.deepEqual(calls, [["wireless", { ...wireless, wifi_enabled: true, ble_enabled: true, board: settings }]]);

  calls.length = 0;
  session.state.link = "ble";
  assert.deepEqual(await operations.saveConfiguration({ settings, force: false, tcpCliEnabled: false, wireless }),
    { tcp_cli_enabled: false });
  assert.deepEqual(calls, [["settings", settings, false, false]]);

  calls.length = 0;
  session.state.link = "usb";
  await operations.saveConfiguration({ settings, force: false, wireless: null });
  assert.deepEqual(calls, [["settings", settings, false, undefined]]);
});


test("scan lifecycle reads generation-indexed entries, keeps raw SSIDs and serializes the whole workflow", async () => {
  const setup = await connected();
  radio(setup, { scanEntries: [Uint8Array.from([0, 255, 65]), new TextEncoder().encode("normal")] });
  const pending = setup.client.scanWireless();
  await assert.rejects(setup.client.operations.tcpCliAuthProof(new Uint8Array(16),
    Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8])), says(/unavailable/));
  const entries = await pending;
  assert.equal(entries[0].ssid_hex, "00ff41");
  assert.equal(entries[0].ssid, "\\x00\\xFFA");
  assert.equal(entries[0].rssi, -40);
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [11, 12, 12]);
  assert.deepEqual(setup.sent[1].slice(5, 8), [0, 1, 0]);
  assert.deepEqual(setup.sent[2].slice(5, 8), [1, 1, 0]);
  assert.equal(setup.client.state.pending_wireless, null);
  await setup.client.disconnect();
});


test("tested pair saves without restaging, key staging retains test, "
  + "save confirms exact switches and commit mask", async () => {
  const setup = await connected();
  const raw = Uint8Array.from([65, 0, 255]);
  radio(setup);
  const tested = await setup.client.testWireless({ ssid: "display only", ssid_hex: "4100FF", password: "password" });
  assert.ok(tested.remaining_seconds > 0 && tested.remaining_seconds <= 600);
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 7, 9]);
  assert.deepEqual(setup.sent[0].slice(5, 9), [3, ...raw]);
  const saved = await setup.client.saveWireless({ ssid: tested.ssid, ssid_hex: tested.ssid_hex,
    use_tested_wifi: true, wifi_enabled: true, ble_enabled: true, tcp_cli_enabled: true,
    ble_key: "ab".repeat(32), board: { ...BOARD, radios: 3 } });
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 7, 9, 8, 5]);
  assert.deepEqual(setup.sent.at(-1).slice(1, 9), [1, 5, 5, 2, 200, 0, 1, 7]);
  assert.equal(saved.ssid_hex, "4100ff");
  assert.equal(saved.ssid, "A\\x00\\xFF");
  assert.equal(setup.client.state.reboot_pending, true);
  assert.equal(setup.client.state.outcome_uncertain, false);
  assert.equal(setup.pendingStore.getItem("picoforge.direct.pending-operation"), null);
  await setup.client.disconnect();
});


test("password omission is safe only for the saved raw SSID; "
  + "explicit open/forget and cancel are supported", async () => {
  const setup = await connected();
  radio(setup);
  await assert.rejects(setup.client.testWireless({ ssid: "different" }), says(/explicit password/));
  assert.equal(setup.sent.length, 0);
  await setup.client.testWireless({ ssid: "home" });
  assert.deepEqual(setup.sent.map(bytes => bytes[3]), [6, 9]);
  await setup.client.cancelWireless();
  assert.equal(setup.sent.at(-1)[3], 10);
  assert.equal(setup.client.state.wireless.staged_mask, 0);
  await setup.client.testWireless({ ssid: "open", password: "" });
  assert.deepEqual(setup.sent.at(-2).slice(3, 7), [7, 0, 0, 0]);
  await setup.client.cancelWireless();
  await setup.client.saveWireless({ ssid: "", password: "", wifi_enabled: false, ble_enabled: false, board: BOARD });
  assert.deepEqual(setup.sent.slice(-2).map(bytes => bytes[3]), [6, 5], "empty SSID implicitly clears password");
  await setup.client.disconnect();
});


test("failed tests leave discardable staging but do not claim a tested pair", async () => {
  const setup = await connected(); radio(setup, { testFailure: true });
  await assert.rejects(setup.client.testWireless({ ssid: "home", password: "password" }), says(/Password rejected/));
  assert.equal(setup.client.state.outcome_uncertain, false);
  assert.equal(setup.client.state.pending_wireless, null);
  await setup.client.cancelWireless();
  await setup.client.disconnect();
});


test("malformed save settings and credentials reject before the first SET", async () => {
  const setup = await connected(); radio(setup);
  const base = { ssid: "home", wifi_enabled: true, ble_enabled: false, board: { ...BOARD, radios: 1 } };
  for (const change of [{ board: { ...base.board, system_clock_mhz: undefined } },
    { board: { ...base.board, sdcard_profile: 2 } }, { use_tested_wifi: 1 }, { tcp_cli_enabled: 1 },
    { ble_key: "0".repeat(63) }, { password: "short" }, { password: "é".repeat(8) }, { ssid_hex: "not hex" }]) {
    await assert.rejects(setup.client.saveWireless({ ...base, ...change }));
    assert.equal(setup.sent.length, 0);
  }
  await setup.client.disconnect();
});

