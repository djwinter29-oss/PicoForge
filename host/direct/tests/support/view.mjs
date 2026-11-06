import { createDashboardView } from "../../web/js/ui/dashboard-view.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../../web/js/protocol/codec.mjs";
import { identity, config, selfTest } from "./fixtures.mjs";

export function withDocument(run) {
  const saved = globalThis.document;
  const elements = new Map();
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, { dataset: {}, textContent: "", value: "0", open: false,
      valueAsNumber: 32, setAttribute(name, value) { this[name] = value; },
      replaceChildren(...children) { this.children = children; this.replaceCount = (this.replaceCount ?? 0) + 1; } });
    return elements.get(selector);
  };
  globalThis.document = { querySelector: element, createElement: () => ({ children: [],
    append(...children) { this.children.push(...children); } }) };
  const renderStatus = createDashboardView();
  try { run(element, renderStatus); } finally { globalThis.document = saved; }
}

export function snapshot() {
  const hardware = decodeIdentityReport(identity());
  return { connected: true, hardware, settings: decodeConfigReport(config(), hardware),
    freshness: { 3: "fresh", 10: "fresh", 11: "fresh" }, self_test: decodeSelfTestReport(selfTest()),
    memory: { sample_valid: true, initialized: true, configured: true, presence_known: true, present: true },
    msc: { enabled: false, transition_pending: false }, filesystem_format: "exfat" };
}

