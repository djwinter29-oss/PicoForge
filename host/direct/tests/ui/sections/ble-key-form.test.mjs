// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import assert from "node:assert/strict";
import test from "node:test";
import { bindBleKeyForm } from "../../../web/js/ui/sections/ble-key-form.mjs";
import { createDocument } from "../../support/dom.mjs";

test("Bluetooth key drafts own generation, save outcomes, retention and explicit removal", async context => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  let bytes, configured = false, enabled = false, changes = 0;
  context.mock.method(crypto, "getRandomValues", value => { bytes = value; return value.fill(1); });
  const key = bindBleKeyForm({
    signal: () => new AbortController().signal, configured: () => configured,
    enable: () => { enabled = true; }, changed: () => changes++, clipboard: null,
  });
  try {
    key.open();
    assert.equal(el("#direct-ble-dialog").open, true);
    assert.match(el("#direct-ble-status").textContent, /No Control key/);
    assert.equal(key.request(true), undefined);
    await fire("#direct-ble-generate", "click");
    assert.equal(bytes.every(value => value === 0), true, "generation wipes the temporary key bytes");
    assert.equal(key.request(true), "01".repeat(32));
    assert.equal(key.dirty(), true);
    assert.equal(enabled, true);
    assert.equal(changes, 1);
    assert.equal(el("#direct-ble-secret").type, "text");
    await fire("#direct-ble-cancel", "click");
    assert.equal(el("#direct-ble-dialog").open, false);
    assert.equal(key.dirty(), true, "closing retains the replacement until save, disable or reset");
    key.finish(false, "Save failed");
    assert.equal(key.canSave(), false);
    assert.equal(el("#direct-ble-secret").value, "");
    assert.equal(el("#direct-ble-secret").type, "password");
    assert.equal(el("#direct-ble-status").textContent, "Save failed");
    assert.match(key.saveBlockReason(), /replacement Control key or disable/);
    key.clear();
    assert.equal(key.canSave(), true, "abandoning the failed replacement releases the Save blocker");
    assert.equal(key.request(true), undefined, "abandoning retains the existing saved key");
    configured = true;
    assert.equal(key.request(false), "", "disabling removes a saved key without requiring regeneration");
    configured = false;
    await fire("#direct-ble-generate", "click");
    assert.equal(key.canSave(), true);
    key.finish(true);
    configured = true;
    assert.equal(key.request(true), undefined, "no draft retains the existing key");
    assert.equal(key.request(false), "", "disabling explicitly removes the existing key");
    key.open();
    assert.match(el("#direct-ble-status").textContent, /cannot be read back/);
    key.reset();
    assert.equal(key.dirty(), false);
    assert.equal(key.canSave(), true);
    assert.equal(el("#direct-ble-dialog").open, false);
  } finally { key.reset(); globalThis.document = previous; }
});

test("Bluetooth replacement failures block Save until regenerated or explicitly disabled", async () => {
  const previous = globalThis.document;
  const { document, element: el, fire } = createDocument();
  globalThis.document = document;
  const key = bindBleKeyForm({
    signal: () => new AbortController().signal, configured: () => true, enable() {}, changed() {}, clipboard: null,
  });
  try {
    await fire("#direct-ble-generate", "click");
    key.finish(false, "Save failed");
    assert.equal(key.canSave(), false);
    assert.equal(key.request(true), undefined);
    assert.equal(el("#direct-ble-secret").value, "");
    await fire("#direct-ble-generate", "click");
    assert.equal(key.canSave(), true);
    assert.equal(key.request(true).length, 64);
    key.finish(false, "Save failed again");
    key.clear();
    assert.equal(key.saveBlockReason(), null);
    assert.equal(key.request(false), "");
  } finally { key.reset(); globalThis.document = previous; }
});

test("Bluetooth clipboard feedback is fenced by the coordinator lifecycle on success and failure", async () => {
  const previous = globalThis.document;
  try {
    for (const outcome of ["success", "failure", "reset-success", "reset-failure", "unavailable"]) {
      const { document, element: el, fire } = createDocument();
      globalThis.document = document;
      let controller = new AbortController();
      let resolveCopy, rejectCopy, copied;
      const key = bindBleKeyForm({
        signal: () => controller.signal, configured: () => false, enable() {}, changed() {},
        clipboard: outcome === "unavailable" ? null : {
          writeText(value) {
            copied = value;
            return new Promise((resolve, reject) => { resolveCopy = resolve; rejectCopy = reject; });
          },
        },
      });

      el("#direct-ble-secret").value = "ab".repeat(32);
      const copying = fire("#direct-ble-copy", "click");
      if (outcome !== "unavailable") {
        assert.equal(copied, "ab".repeat(32));
        if (outcome.startsWith("reset")) {
          controller.abort();
          controller = new AbortController();
          key.reset();
          el("#direct-ble-status").textContent = "new draft";
        }
        if (outcome.endsWith("failure")) rejectCopy(new Error("Clipboard denied"));
        else resolveCopy();
      }
      await copying;
      const message = el("#direct-ble-status").textContent;
      if (outcome.startsWith("reset")) assert.equal(message, "new draft");
      else if (outcome === "success") assert.match(message, /Key copied/);
      else assert.match(message, /Could not copy key/);
      key.reset();
    }
  } finally { globalThis.document = previous; }
});

test("clipboard completions cannot overwrite feedback from a newer key draft or save outcome", async () => {
  const previous = globalThis.document;
  try {
    for (const change of ["generate", "disable", "saved", "save-failed", "reset"]) {
      for (const outcome of ["resolve", "reject"]) {
        const { document, element: el, fire } = createDocument();
        globalThis.document = document;
        const controller = new AbortController();
        let resolveCopy, rejectCopy, copied;
        const key = bindBleKeyForm({
          signal: () => controller.signal, configured: () => true, enable() {}, changed() {},
          clipboard: { writeText(value) {
            copied = value;
            return new Promise((resolve, reject) => { resolveCopy = resolve; rejectCopy = reject; });
          } },
        });
        try {
          await fire("#direct-ble-generate", "click");
          const copying = fire("#direct-ble-copy", "click");
          assert.equal(copied.length, 64);
          if (change === "generate") await fire("#direct-ble-generate", "click");
          else if (change === "disable") key.clear();
          else if (change === "saved") key.finish(true);
          else if (change === "save-failed") key.finish(false, "Save failed; regenerate the key");
          else key.reset(); // The draft revision must fence even without a lifecycle abort.
          const before = {
            message: el("#direct-ble-status").textContent,
            secret: el("#direct-ble-secret").value,
            dirty: key.dirty(),
            canSave: key.canSave(),
          };
          if (outcome === "resolve") resolveCopy();
          else rejectCopy(new Error("Old clipboard failure"));
          await copying;
          assert.deepEqual({
            message: el("#direct-ble-status").textContent,
            secret: el("#direct-ble-secret").value,
            dirty: key.dirty(),
            canSave: key.canSave(),
          }, before, `${change}/${outcome}`);
        } finally { key.reset(); }
      }
    }
  } finally { globalThis.document = previous; }
});
