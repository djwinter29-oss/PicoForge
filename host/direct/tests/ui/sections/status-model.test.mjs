import assert from "node:assert/strict";
import test from "node:test";
import { deriveDashboardViewModel } from "../../../web/js/ui/sections/status-model.mjs";

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
