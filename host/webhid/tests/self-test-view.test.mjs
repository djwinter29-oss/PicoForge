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
      element("#self-test-size").valueAsNumber = 1024;
      render(snapshot);
      assert.equal(element("#self-test-start").disabled, false);
      for (const invalid of [0, 1025, 1.5, NaN]) {
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
  test(`${name} displays NOR metadata and permits a one MiB benchmark`, () => {
    const elements = new Map();
    const element = selector => {
      if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", valueAsNumber: 1 });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    try {
      render({ connected: true, memory: {
        configured: true, initialized: true, presence_known: true, present: true,
        component_type: "nor-flash", mode: "spi", bus_clock_hz: 992063,
        capacity_bytes: 3000000, physical_capacity_bytes: 16777216,
        manufacturer: { id: 0xef, product_name: "W25Q128", jedec_id: "ef4018", serial_number: null },
      }, filesystem_format: "fat12", self_test: { state: "idle" } });
      assert.equal(element("#memory-state").textContent, "Ready");
      assert.equal(element("#memory-type").textContent, "nor-flash");
      assert.match(element("#memory-manufacturer").textContent, /Winbond/);
      assert.match(element("#memory-product").textContent, /W25Q128.*ef4018/);
      assert.match(element("#memory-size").textContent, /usable.*16.00 MiB physical/);
      assert.equal(element("#memory-serial").textContent, "Not reported");
      assert.equal(element("#self-test-start").disabled, false);
      render({ connected: true, memory: { configured: true, initialized: true,
        component_type: "nor-flash", device_uid: "0x0123456789abcdef",
        jedec_id: "ef4018" } });
      assert.equal(element("#memory-serial").textContent, "0x0123456789abcdef");
    } finally { globalThis.document = previous; }
  });

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
  test(`${name} renders interval CPU and reserved RAM, including legacy and disconnected states`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#runtime-clock", "system clock is shown only in the identity row");
      assert.notEqual(selector, "#sequence", "report sequence is protocol-only diagnostics");
      if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", valueAsNumber: 32 });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    try {
      const runtime = { system_clock_hz: 250_000_000, cpu_usage_basis_points: 1234,
        ram_used_bytes: 65536, ram_total_bytes: 532480 };
      render({ connected: true, runtime });
      assert.equal(element("#identity-clock").textContent, "250 MHz");
      assert.equal(element("#cpu-usage").textContent, "12.3%");
      assert.equal(element("#ram-usage").textContent, "64.0 / 520 KiB (12.3%)");
      for (const cpu of [0, 10000]) {
        runtime.cpu_usage_basis_points = cpu;
        render({ connected: true, runtime });
        assert.equal(element("#cpu-usage").textContent, `${(cpu / 100).toFixed(1)}%`);
      }
      runtime.cpu_usage_basis_points = null;
      render({ connected: true, runtime });
      assert.equal(element("#cpu-usage").textContent, "--");
      assert.match(element("#ram-usage").textContent, /520 KiB/);
      for (const snapshot of [{ connected: true, runtime: { system_clock_hz: 125_000_000 } },
        { connected: false }]) {
        render(snapshot);
        assert.equal(element("#cpu-usage").textContent, "--");
        assert.equal(element("#ram-usage").textContent, "--");
      }
    } finally {
      globalThis.document = previous;
    }
  });

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