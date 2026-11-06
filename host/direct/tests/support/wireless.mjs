// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import { fakeConnection, emit, storage, selfTest, result } from "./fixtures.mjs";

export const BOARD = { system_clock_mhz: 200, sdcard_profile: 1, radios: 0 };
export function status({ state = 0, flags = 0, staged = 0, ssid = [], scanState = 0, scanGeneration = 0,
  count = 0, testGeneration = 0, detail = 0 } = {}) {
  const bytes = new Uint8Array(48);
  bytes.set([1, state, detail, 0, flags]);
  if ([2, 5].includes(state)) bytes.set([192, 168, 1, 9], 5);
  bytes[9] = ssid.length;
  bytes.set(ssid, 10);
  bytes.set([scanState, scanGeneration & 255, scanGeneration >> 8, count, testGeneration, staged], 42);
  return bytes;
}

export async function connected() {
  let now = 0;
  const setup = fakeConnection({ capabilities: 128 | 55, now: () => now,
    sleep: async milliseconds => { now += milliseconds; } });
  await setup.client.connect();
  emit(setup.device, 10, storage()); emit(setup.device, 11, selfTest());
  return setup;
}

/** Stateful report double: opcodes mutate staging, accepted test/scan generations, and the last result slot. */
export function radio(setup, { raw = new TextEncoder().encode("home"), scanEntries = [], testFailure = false } = {}) {
  let stagedMask = 0, stagedState = 0, tested = false, testGeneration = 0, scanGeneration = 0, scanning = false;
  let saved = raw.slice(), candidate = raw.slice();
  let last = result(0, 0, 0);
  const receive = setup.device.receiveFeatureReport;
  setup.device.receiveFeatureReport = async id => {
    if (id === 12) return status({ ssid: candidate, state: testGeneration ? testFailure ? 6 : 5 : 0,
      flags: (saved.length ? 1 : 0) | stagedMask << 4 | (tested ? 128 : 0),
      staged: stagedState, testGeneration, detail: testFailure ? 3 : 0,
      scanGeneration, scanState: scanning ? 2 : 0, count: scanning ? scanEntries.length : 0 });
    if (id === 20) return last;
    return receive(id);
  };
  const send = setup.device.sendFeatureReport;
  setup.device.sendFeatureReport = async (id, bytes) => {
    assert.equal(id, 20); assert.equal(bytes.length, 68);
    await send(id, bytes);
    const op = bytes[2], args = bytes.subarray(4);
    last = result(bytes[1], op);
    if (op === 6) {
      candidate = args.slice(1, args[0] + 1);
      stagedMask |= 1; tested = false;
      if (!candidate.length) { stagedMask |= 2; stagedState |= 3; }
    } else if (op === 7) {
      stagedMask |= 2; tested = false;
      stagedState = args[0] ? stagedState & ~2 : stagedState | 2;
    } else if (op === 8) {
      stagedMask |= 4; stagedState = args[0] ? stagedState & ~4 : stagedState | 4;
    } else if (op === 9) { testGeneration = testGeneration % 255 + 1; tested = !testFailure; }
    else if (op === 10) { stagedMask = 0; stagedState = 0; tested = false; testGeneration = 0; candidate = saved; }
    else if (op === 11) { scanning = true; scanGeneration = scanGeneration % 65535 + 1; }
    else if (op === 12) {
      const entry = scanEntries[args[0]];
      last.set([scanGeneration & 255, scanGeneration >> 8, args[0], entry.length], 5);
      last.set(entry, 9); last[41] = 216; last[42] = 5;
    } else if (op === 5) {
      last.set(args.subarray(0, 4), 5); last[9] = stagedMask;
      saved = candidate; stagedMask = 0; stagedState = 0; tested = false;
    } else if (op === 13) last.fill(42, 5, 21);
  };
  return { replaceResult(value) { last = value; } };
}

