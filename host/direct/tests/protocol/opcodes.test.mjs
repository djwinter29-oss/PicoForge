import assert from "node:assert/strict";
import test from "node:test";
import { decodeSettings, encodeSettings } from "../../web/js/protocol/opcodes.mjs";

test("settings codec round-trips the four save-settings argument bytes", () => {
  const settings = { system_clock_mhz: 300, sdcard_profile: 1, radios: 3, tcp_cli_enabled: true };
  const args = encodeSettings(settings, true);
  assert.deepEqual(args, [44, 1, 1, 7]);
  assert.deepEqual(decodeSettings(args), settings);
  assert.deepEqual(decodeSettings(encodeSettings({ ...settings, radios: undefined }, false)),
    { ...settings, radios: 0, tcp_cli_enabled: false });
});
