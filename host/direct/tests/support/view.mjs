// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { createDashboardView } from "../../web/js/ui/dashboard-view.mjs";
import { decodeIdentityReport, decodeConfigReport, decodeSelfTestReport } from "../../web/js/protocol/codec.mjs";
import { identity, config, selfTest } from "./fixtures.mjs";
import { createDocument } from "./dom.mjs";

export function withDocument(run) {
  const saved = globalThis.document;
  const { document, element } = createDocument();
  for (const id of ["board-uart", "board-storage-pins"]) {
    const table = element(`#${id}`);
    const replace = table.replaceChildren;
    table.replaceChildren = function (...children) {
      replace.call(this, ...children);
      this.replaceCount = (this.replaceCount ?? 0) + 1;
    };
  }
  globalThis.document = document;
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
