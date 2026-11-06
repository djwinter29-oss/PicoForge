import assert from "node:assert/strict";
import test from "node:test";
import { deriveDashboardViewModel } from "../js/ui/sections/status-model.mjs";
import { deriveStorageViewModel } from "../js/ui/sections/storage.mjs";
import { deriveSelfTestViewModel } from "../js/ui/sections/self-test.mjs";

test("dashboard view model derives connection messaging, readiness, and same-origin console URL", () => {
  const disconnected = deriveDashboardViewModel({});
  assert.equal(disconnected.connection_state, "disconnected");
  assert.equal(disconnected.connection_label, "Disconnected");
  assert.equal(disconnected.status_message, "Select a PicoForge device");
  assert.equal(disconnected.console_url, null);

  const connected = deriveDashboardViewModel({
    connected: true,
    hardware: { capabilities: 0 },
    wireless: { wifi_enabled: true, wifi_state: "connected", tcp_cli_started: true, ipv4: "192.168.1.2" },
  });

  assert.equal(connected.connection_state, "connected");
  assert.equal(connected.connection_label, "Connected");
  assert.equal(connected.console_url, "console.html?ipv4=192.168.1.2");

  const error = deriveDashboardViewModel({ connected: true, error: "Connection failed" });
  assert.equal(error.connection_state, "error");
  assert.equal(error.connection_label, "Attention");
  assert.equal(error.status_message, "Connection failed");
});

test("storage view models retain only their explicitly supplied display history", () => {
  const snapshot = { connected: true, memory: {
    configured: true, initialized: true, sample_valid: true, capacity_bytes: 1048576,
  } };
  const previous = deriveStorageViewModel(snapshot).retained;
  Object.freeze(previous);
  const busy = { ...snapshot, memory: { configured: true, sample_valid: false } };
  assert.equal(deriveStorageViewModel(busy, previous).text["memory-size"], "1.00 MiB");
  assert.equal(deriveStorageViewModel(busy).text["memory-size"], "--",
    "a separate view cannot inherit another view's retained card details");
  assert.equal(deriveStorageViewModel({ connected: false }).retained.details, null);
});

test("self-test view models fail closed without a fresh controllable report", () => {
  const model = deriveSelfTestViewModel({ connected: true }, 32);
  assert.equal(model.text["self-test-state"], "Unavailable");
  assert.equal(model.startDisabled, true);
  assert.equal(model.cancelDisabled, true);
});
