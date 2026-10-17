import assert from "node:assert/strict";
import test from "node:test";
import { renderSelfTest as renderDirect, renderStatus as renderDirectStatus } from "../js/view.mjs";
import { renderSelfTest as renderShared, renderStatus as renderSharedStatus } from "../../web/js/dashboard.mjs";

for (const [name, render] of [["WebHID", renderDirect], ["shared dashboard", renderShared]]) {
  test(`${name} gates writes, renders throughput, and permits active cancellation`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#self-test-consent", "the removed consent input must not be used");
      if (!elements.has(selector)) elements.set(selector, { textContent: "", value: 0, valueAsNumber: 32 });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    const status = { state: "idle", active: false, target_bytes: 0, written_bytes: 0, read_bytes: 0,
      write_elapsed_ms: 0, read_elapsed_ms: 0, progress_percent: 0, write_bytes_per_second: null,
      read_bytes_per_second: null, error: 0, request_error: 0, mismatch_count: 0 };
    const snapshot = { connected: true, memory: { initialized: true }, filesystem_format: "exfat", msc: { enabled: false }, self_test: status };
    try {
      render(snapshot);
      assert.equal(element("#self-test-start").disabled, false);
      for (const invalid of [0, 129, 1.5, NaN]) {
        element("#self-test-size").valueAsNumber = invalid;
        render(snapshot);
        assert.equal(element("#self-test-start").disabled, true);
      }
      element("#self-test-size").valueAsNumber = 32;
      for (const field of ["busy", "self_test_command_pending"]) {
        snapshot[field] = true;
        render(snapshot);
        assert.equal(element("#self-test-start").disabled, true);
        snapshot[field] = false;
      }
      for (const format of [null, undefined, "unknown"]) {
        snapshot.filesystem_format = format;
        render(snapshot);
        assert.equal(element("#self-test-start").disabled, true);
      }
      for (const format of ["fat12", "fat16", "fat32", "exfat"]) {
        snapshot.filesystem_format = format;
        render(snapshot);
        assert.equal(element("#self-test-start").disabled, false);
      }
      status.request_error = 11;
      render(snapshot);
      assert.match(element("#self-test-message").textContent, /Filesystem not mounted/);
      status.request_error = 0;
      snapshot.self_test = { ...status, state: "writing", active: true, target_bytes: 33554432,
        written_bytes: 16777216, write_elapsed_ms: 16000, write_bytes_per_second: 1048576, progress_percent: 25 };
      render(snapshot);
      assert.equal(element("#self-test-start").disabled, true);
      assert.equal(element("#self-test-cancel").disabled, false);
      assert.equal(element("#self-test-write-speed").textContent, "1.00 MiB/s");
      assert.equal(element("#self-test-progress").value, 25);
      snapshot.self_test = { ...status, state: "failed", error: 10, file_remains: true };
      render(snapshot);
      assert.match(element("#self-test-message").textContent, /PFTEST.BIN remains/);
      snapshot.msc.enabled = true;
      render(snapshot);
      assert.equal(element("#self-test-start").disabled, true);
      snapshot.msc = { enabled: false, transition_pending: true };
      render(snapshot);
      assert.equal(element("#self-test-start").disabled, true);
    } finally {
      globalThis.document = previous;
    }
  });
}

for (const [name, render] of [["WebHID", renderDirectStatus], ["shared dashboard", renderSharedStatus]]) {
  test(`${name} reports SD-card presence in Live Report and resolves known manufacturer names`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#memory-presence", "presence belongs in Live Report");
      if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", valueAsNumber: 32, checked: false });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    const manufacturer = {
      ...(name === "WebHID" ? { id: 65 } : { manufacturer_id: 65 }),
      oem_id: "42",
      product_name: "SD16G",
      product_revision: "2.0",
      manufacture_year: 2013,
      manufacture_month: 10,
      serial_number: 1,
    };
    try {
      const snapshot = { connected: true, memory: { configured: true, initialized: true,
        presence_known: true, present: true, manufacturer } };
      render(snapshot);
      assert.equal(element("#card-presence").textContent, "Present");
      assert.equal(element("#memory-manufacturer").textContent, "Kingston (0x41)");
      assert.equal(element("#self-test-state").textContent, "Unavailable");
      snapshot.memory.present = false;
      render(snapshot);
      assert.equal(element("#card-presence").textContent, "Absent");
      snapshot.memory.presence_known = false;
      render(snapshot);
      assert.equal(element("#card-presence").textContent, "Unknown");
      snapshot.memory.configured = false;
      render(snapshot);
      assert.equal(element("#card-presence").textContent, "Not configured");
      snapshot.memory = { manufacturer: {
        ...manufacturer,
        id: 3,
        manufacturer_id: 3,
      } };
      render(snapshot);
      assert.equal(element("#memory-manufacturer").textContent, "SanDisk (0x03)");
      snapshot.memory.manufacturer.id = 0x77;
      snapshot.memory.manufacturer.manufacturer_id = 0x77;
      render(snapshot);
      assert.equal(element("#memory-manufacturer").textContent, "Unknown manufacturer (0x77)");
    } finally {
      globalThis.document = previous;
    }
  });
}

for (const [name, render] of [["WebHID", renderDirectStatus], ["shared dashboard", renderSharedStatus]]) {
  test(`${name} renders firmware release and does not confuse legacy protocol version with release`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#self-test-consent");
      if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", valueAsNumber: 32 });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    try {
      for (const firmwareVersion of ["0.1.0", "12.34.255", null, undefined]) {
        render({ connected: true, hardware: { version: firmwareVersion ? 3 : 2, firmware_version: firmwareVersion },
          runtime: { system_clock_hz: 200_000_000 } });
        assert.equal(element("#firmware-version").textContent, firmwareVersion ?? "Unknown");
        assert.equal(element("#identity-clock").textContent, "200 MHz");
      }
    } finally {
      globalThis.document = previous;
    }
  });
}