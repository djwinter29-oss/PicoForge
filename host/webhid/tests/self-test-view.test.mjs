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
        read_bytes_per_second: null, error: 0, request_error: 0, mismatch_count: 0,
        volume_mounted: true };
    const snapshot = { connected: true, memory: { initialized: true }, filesystem_format: "exfat", msc: { enabled: false }, self_test: status };
    try {
      render(snapshot);
      assert.equal(element("#self-test-message").textContent, "");
      assert.equal(element("#self-test-start").disabled, false);
      assert.doesNotMatch(element("#self-test-message").textContent, /Filesystem remains mounted/);
      status.state = "passed";
      render(snapshot);
      assert.equal(element("#self-test-message").textContent, "");
      status.state = "cancelled";
      render(snapshot);
      assert.equal(element("#self-test-message").textContent, "");
      status.state = "idle";
      render(snapshot);
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
  test(`${name} hides Storage benchmark during host ownership and hides both storage sections when absent`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#card-presence", "SD presence is not a Runtime metric");
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, textContent: "", valueAsNumber: 32, checked: false, hidden: false,
        setAttribute(name, value) { this[name] = value; },
      });
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
        presence_known: true, present: true, manufacturer },
      msc: { enabled: false, transition_pending: false } };
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, false);
      snapshot.msc.enabled = true;
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, true);
      snapshot.msc.enabled = false;
      snapshot.msc.transition_pending = true;
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, true);
      snapshot.msc.transition_pending = false;
      assert.equal(element("#memory-note").textContent, "");
      assert.equal(element("#memory-manufacturer").textContent, "Kingston (0x41)");
      assert.equal(element("#memory-serial").textContent, "00000001");
      assert.equal(element("#self-test-state").textContent, "Unavailable");
      snapshot.memory.present = false;
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, true);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, true);
      snapshot.memory.presence_known = false;
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, false);
      snapshot.memory.configured = false;
      render(snapshot);
      assert.equal(element('[aria-labelledby="memory-title"]').hidden, false);
      assert.equal(element('[aria-labelledby="self-test-title"]').hidden, false);
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

test("shared dashboard preserves unsaved boot settings and blocks writes while storage is owned", () => {
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, {
      dataset: {}, textContent: "", value: "0", valueAsNumber: 32,
      setAttribute(name, value) { this[name] = value; },
    });
    return elements.get(selector);
  };
  const previous = globalThis.document;
  globalThis.document = { querySelector: element };
  try {
    const snapshot = {
      connected: true,
      settings_supported: true,
      settings: { system_clock_mhz: 200, sdio_bus_mhz: 25, spi_bus_mhz: 50 },
      memory: { mode: "spi" },
      msc: { enabled: false, transition_pending: false },
      self_test: { active: false },
    };
    renderSharedStatus(snapshot);
    assert.equal(element("#settings-clock").value, "200");
    assert.equal(element("#settings-storage").value, "50");
    assert.equal(element("#settings-clock").disabled, false);
    assert.equal(element("#settings-save").disabled, true);

    snapshot.settings_dirty = true;
    element("#settings-clock").value = "250";
    renderSharedStatus(snapshot);
    assert.equal(element("#settings-clock").value, "250");
    assert.equal(element("#settings-storage").value, "50");
    assert.equal(element("#settings-save").disabled, false);

    snapshot.msc.enabled = true;
    renderSharedStatus(snapshot);
    assert.equal(element("#settings-clock").disabled, true);
    assert.equal(element("#settings-save").disabled, true);
    assert.match(element("#settings-note").textContent, /Disable the USB drive/);
  } finally {
    globalThis.document = previous;
  }
});

for (const [name, render] of [["WebHID", renderDirectStatus], ["shared dashboard", renderSharedStatus]]) {
  test(`${name} limits system clock options to the detected MCU`, () => {
    const elements = new Map();
    const element = selector => {
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, textContent: "", value: "0", valueAsNumber: 32,
        setAttribute(name, value) { this[name] = value; },
        replaceChildren(...children) {
          this.children = children;
          if (!children.some(child => child.value === this.value)) this.value = children[0]?.value ?? "";
        },
      });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = {
      querySelector: element,
      createElement: () => ({ value: "", textContent: "" }),
    };
    try {
      const snapshot = {
        connected: true,
        hardware: { mcu: "RP2040" },
        settings_supported: true,
        settings: { system_clock_mhz: 0, sdio_bus_mhz: 0, spi_bus_mhz: 0 },
        msc: { enabled: false },
        self_test: { active: false },
      };
      render(snapshot);
      const clock = element("#settings-clock");
      assert.deepEqual(clock.children.map(option => option.value), ["0", "150", "200", "250"]);
      assert.equal(clock.children[0].textContent, "Default (125 MHz)");

      snapshot.hardware.mcu = "RP2350";
      render(snapshot);
      assert.deepEqual(clock.children.map(option => option.value), ["0", "200", "250"]);
      assert.equal(clock.children[0].textContent, "Default (150 MHz)");

      snapshot.settings.system_clock_mhz = 150;
      render(snapshot);
      assert.equal(clock.children.at(-1).value, "150");
      assert.equal(clock.children.at(-1).disabled, true);
      assert.equal(clock.children.at(-1).textContent, "150 MHz (saved; unsupported)");
    } finally {
      globalThis.document = previous;
    }
  });
}

for (const [name, render] of [["WebHID", renderDirectStatus], ["shared dashboard", renderSharedStatus]]) {
  test(`${name} reports LED state on the toggle control`, () => {
    const elements = new Map();
    const element = selector => {
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, textContent: "", valueAsNumber: 32, setAttribute(name, value) { this[name] = value; },
      });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    try {
      const button = element("#led-control");
      render({ connected: true, board_status: { led_available: true, led_on: false } });
      assert.equal(button.textContent, "Board LED: Off");
      assert.equal(button.dataset.state, "off");
      assert.equal(button["aria-pressed"], "false");
      assert.match(button.title, /Turn onboard LED on/);
      render({ connected: true, board_status: { led_available: true, led_on: true } });
      assert.equal(button.textContent, "Board LED: On");
      assert.equal(button.dataset.state, "on");
      assert.equal(button["aria-pressed"], "true");
      assert.match(button.title, /Turn onboard LED off/);
      render({ connected: true, board_status: { led_available: false, led_on: false } });
      assert.equal(button.textContent, "Board LED: --");
      assert.equal(button.dataset.state, "unknown");
      assert.equal(button["aria-pressed"], "false");
    } finally {
      globalThis.document = previous;
    }
  });

  test(`${name} renders interval CPU and reserved RAM, including legacy and disconnected states`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#runtime-clock", "system clock is shown only in the identity row");
      assert.notEqual(selector, "#sequence", "report sequence is protocol-only diagnostics");
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, textContent: "", valueAsNumber: 32,
        setAttribute(name, value) { this[name] = value; },
      });
      return elements.get(selector);
    };
    const previous = globalThis.document;
    globalThis.document = { querySelector: element };
    try {
      const runtime = { system_clock_hz: 250_000_000,
        cpu_usage_core0_basis_points: 1234, cpu_usage_core1_basis_points: 5678,
        ram_used_bytes: 65536, ram_total_bytes: 532480 };
      render({ connected: true, runtime });
      assert.equal(element("#identity-clock").textContent, "250 MHz");
      assert.equal(element("#cpu-usage-core0").textContent, "12.3%");
      assert.equal(element("#cpu-usage-core1").textContent, "56.8%");
      assert.equal(element("#ram-reserved").textContent, "64.0 KiB (12.3%)");
      assert.equal(element("#ram-total").textContent, "520 KiB");
      for (const cpu of [0, 10000]) {
        runtime.cpu_usage_core0_basis_points = cpu;
        runtime.cpu_usage_core1_basis_points = cpu;
        render({ connected: true, runtime });
        assert.equal(element("#cpu-usage-core0").textContent, `${(cpu / 100).toFixed(1)}%`);
        assert.equal(element("#cpu-usage-core1").textContent, `${(cpu / 100).toFixed(1)}%`);
      }
      runtime.cpu_usage_core0_basis_points = null;
      runtime.cpu_usage_core1_basis_points = null;
      render({ connected: true, runtime });
      assert.equal(element("#cpu-usage-core0").textContent, "--");
      assert.equal(element("#cpu-usage-core1").textContent, "--");
      assert.equal(element("#ram-total").textContent, "520 KiB");
      for (const snapshot of [{ connected: true, runtime: { system_clock_hz: 125_000_000 } },
        { connected: false }]) {
        render(snapshot);
        assert.equal(element("#cpu-usage-core0").textContent, "--");
        assert.equal(element("#cpu-usage-core1").textContent, "--");
        assert.equal(element("#ram-reserved").textContent, "--");
        assert.equal(element("#ram-total").textContent, "--");
      }
    } finally {
      globalThis.document = previous;
    }
  });

  test(`${name} renders firmware release and does not confuse legacy protocol version with release`, () => {
    const elements = new Map();
    const element = selector => {
      assert.notEqual(selector, "#self-test-consent");
      if (!elements.has(selector)) elements.set(selector, {
        dataset: {}, textContent: "", valueAsNumber: 32,
        setAttribute(name, value) { this[name] = value; },
      });
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