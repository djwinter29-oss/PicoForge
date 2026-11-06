// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** Direct-only DOM fixture with additive listeners and explicit asynchronous event dispatch. */
export function createDocument() {
  const elements = new Map();
  const makeElement = () => ({
    namespaceURI: "http://www.w3.org/1999/xhtml",
    value: "0", textContent: "", dataset: {}, children: [], open: false, disabled: false, valueAsNumber: 32,
    options: ["0", "1", "configure"].map(value => ({ value })),
    listeners: new Map(),
    label: { toggleAttribute(name, value) { this[name] = value; } },
    closest() { return this.label; },
    addEventListener(type, handler) {
      const listeners = this.listeners.get(type) ?? [];
      listeners.push(handler);
      this.listeners.set(type, listeners);
    },
    dispatchEvent(event) {
      for (const handler of this.listeners.get(event.type) ?? []) handler(event);
      return !event.defaultPrevented;
    },
    setAttribute(name, value) { this[name] = value; },
    querySelector(selector) { return this.children.find(child => child.localName === selector) ?? null; },
    querySelectorAll(selector) { return this.children.filter(child => child.localName === selector); },
    append(...children) { this.children.push(...children); },
    replaceChildren(...children) { this.children = children; },
    showModal() { this.open = true; },
    close() {
      if (!this.open) return;
      this.open = false;
      this.dispatchEvent(new Event("close"));
    },
    focus() { this.dispatchEvent(new Event("focus")); },
  });
  const element = selector => {
    const tag = selector.match(/^([a-z]+)#/)?.[1];
    if (tag) selector = selector.slice(tag.length);
    if (!elements.has(selector)) elements.set(selector, makeElement());
    if (tag) elements.get(selector).localName ??= tag;
    return elements.get(selector);
  };
  return {
    document: { querySelector: element, createElement: tag => Object.assign(makeElement(), { localName: tag }) },
    element,
    async fire(selector, type) {
      const event = new Event(type, { cancelable: true });
      // Native dispatch invokes every listener before awaiting any asynchronous continuation.
      const pending = [];
      for (const handler of element(selector).listeners.get(type) ?? []) pending.push(handler(event));
      await Promise.all(pending);
    },
  };
}
