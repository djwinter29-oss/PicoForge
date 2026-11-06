import assert from "node:assert/strict";
import test from "node:test";
import { renderWirelessForm } from "../../../web/js/ui/sections/wireless-form-view.mjs";
import { createDocument } from "../../support/dom.mjs";

test("wireless form presentation derives phase gates, key availability and accurate test summaries", () => {
  const previous = globalThis.document;
  const { document, element: el } = createDocument();
  globalThis.document = document;
  const view = {
    available: true, wirelessReady: true, radioReady: true, tcpReady: true,
    status: { ssid: "saved", ble_key_configured: false },
    form: { phase: "idle", cancelError: null, cancelNeedsReconnect: false },
    wifiStage: { ssid: "tested", use_tested_wifi: true }, remainingSeconds: 9,
    selectedSsidHex: "61", keyDirty: false,
    saveBlockReason: null,
  };
  el("#direct-settings-wifi").value = "1";
  el("#direct-wifi-ssid").value = "tested";
  el("#direct-ble-secret").value = "ab".repeat(32);
  const network = document.createElement("button");
  network.dataset.ssidHex = "61";
  el("#direct-wifi-networks").append(network);
  try {
    const bleOn = el("#direct-settings-ble").options.find(option => option.value === "1");
    renderWirelessForm(view);
    assert.equal(bleOn.disabled, true);
    assert.equal(network["aria-pressed"], "true");
    assert.equal(el("#direct-ble-copy").disabled, false);
    assert.match(el("#direct-wireless-summary").textContent, /expires in 9s/);
    renderWirelessForm({ ...view, keyDirty: true, remainingSeconds: 0,
      saveBlockReason: "Test credentials expired; test the connection again before saving." });
    assert.equal(bleOn.disabled, false);
    assert.match(el("#direct-wireless-summary").textContent, /expired/);
    for (const phase of ["scanning", "testing", "canceling", "cancelFailed"]) {
      renderWirelessForm({ ...view, form: { phase, cancelError: "clear failed", cancelNeedsReconnect: true } });
      assert.equal(network.disabled, true, phase);
      assert.equal(el("#direct-wifi-ssid").disabled, true, phase);
      assert.equal(el("#direct-ble-generate").disabled, true, phase);
      assert.equal(el("#direct-wifi-cancel").disabled, true, phase);
      if (phase === "cancelFailed") assert.match(el("#direct-wireless-summary").textContent, /Reconnect/);
    }
    renderWirelessForm({ ...view, available: false, wirelessReady: false, radioReady: false, tcpReady: false });
    assert.equal(el("#direct-settings-tcp-cli").disabled, true);
    assert.equal(el("#direct-settings-tcp-cli").label.hidden, true);
    assert.equal(el("#direct-wireless-summary").hidden, true);
  } finally { globalThis.document = previous; }
});
