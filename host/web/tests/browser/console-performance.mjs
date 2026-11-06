// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** Measure the actual console module and styles at bounded scrollback, not a proxy renderer. */
async function measureConsole() {
  const { ConsoleTerminal } = await import("/js/console-terminal.mjs");
  const terminal = document.querySelector("#terminal");
  const samples = [];
  let phases = { parse: 0, text: 0, dom: 0, layout: 0, renders: 0 };
  const wrap = (object, name, phase) => {
    const original = object[name];
    object[name] = function (...args) {
      const started = performance.now();
      const result = original.apply(this, args);
      phases[phase] += performance.now() - started;
      if (phase === "dom") phases.renders++;
      return result;
    };
    return () => { object[name] = original; };
  };
  const restores = [
    wrap(ConsoleTerminal.prototype, "write", "parse"),
    wrap(ConsoleTerminal.prototype, "display", "text"),
    wrap(terminal, "replaceChildren", "dom"),
  ];
  const height = Object.getOwnPropertyDescriptor(Element.prototype, "scrollHeight").get;
  Object.defineProperty(terminal, "scrollHeight", { configurable: true, get() {
    const started = performance.now();
    const value = height.call(this);
    phases.layout += performance.now() - started;
    return value;
  } });
  let queued = [];
  let pending = null;
  const respond = body => ({ ok: true, json: async () => body });
  const previousFetch = window.fetch;
  window.fetch = async (_url, options) => {
    if (options.method === "POST") return respond({ session: "performance-session" });
    if (options.method === "DELETE") return respond({ ok: true });
    if (options.method === "PUT") return respond({ ok: true });
    const data = queued.length ? queued.shift() : await new Promise((resolve, reject) => {
      pending = resolve;
      options.signal.addEventListener("abort", () => { pending = null; reject(options.signal.reason); }, { once: true });
    });
    return respond({ data: btoa(data), closed: false });
  };
  const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
  const feed = async chunks => {
    queued.push(...chunks);
    if (pending) { const release = pending; pending = null; release(queued.shift()); }
    for (let attempt = 0; attempt < 100 && (queued.length || !pending); attempt++) await frame();
    if (queued.length || !pending) throw new Error("Console polling did not consume the measurement burst");
    await frame();
  };
  try {
    document.querySelector("#connect").click();
    await feed([("x".repeat(512) + "\r\n").repeat(1000)]);
    for (let sample = 0; sample < 30; sample++) {
      phases = { parse: 0, text: 0, dom: 0, layout: 0, renders: 0 };
      await feed(Array.from({ length: 6 }, (_, index) => `sample ${sample}/${index}`.padEnd(512, "x") + "\r\n"));
      samples.push({ ...phases });
      const lines = terminal.textContent.split("\n");
      if (lines.length !== 1000 || Math.max(...lines.map(line => line.length)) !== 512)
        throw new Error("Console measurement did not retain the full supported scrollback and column bounds");
    }
    const summary = {};
    for (const phase of ["parse", "text", "dom", "layout", "renders"]) {
      const values = samples.map(sample => sample[phase]).sort((a, b) => a - b);
      summary[phase] = { median: values[15], p95: values[28] };
    }
    return summary;
  } finally {
    document.querySelector("#connect").click();
    await new Promise(resolve => setTimeout(resolve, 0));
    window.fetch = previousFetch;
    delete terminal.scrollHeight;
    restores.forEach(restore => restore());
  }
}
