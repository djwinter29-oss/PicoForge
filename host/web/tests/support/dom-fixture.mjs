// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** Minimal additive-listener DOM for the shared dashboard; unknown members are not silently stubbed. */
export function domFixture() {
  const elements = new Map();
  const handlers = new Map();
  const listeners = new Map();
  let document;
  const node = selector => {
    const events = new Map();
    const attributes = {};
    const classes = new Set();
    let textContent = "";
    const element = {
      value: "", valueAsNumber: 32, dataset: {}, style: {}, children: [], attributes,
      get textContent() {
        return this.children.length ? this.children.map(child => child.textContent).join("") : textContent;
      },
      set textContent(value) { textContent = String(value); this.children = []; },
      disabled: false, hidden: false, open: false, type: "", tagName: "DIV",
      scrollHeight: 0, scrollTop: 0, clientHeight: 0,
      options: ["0", "1"].map(value => ({ value })),
      setAttribute(name, value) { attributes[name] = String(value); },
      getAttribute(name) { return attributes[name] ?? null; },
      toggleAttribute(name, force) {
        const enabled = force ?? !Object.hasOwn(attributes, name);
        if (enabled) attributes[name] = "";
        else delete attributes[name];
        this[name] = enabled;
      },
      classList: {
        add(...names) { names.forEach(name => classes.add(name)); },
        remove(...names) { names.forEach(name => classes.delete(name)); },
        toggle(name, force) {
          const enabled = force ?? !classes.has(name);
          if (enabled) classes.add(name); else classes.delete(name);
          return enabled;
        },
      },
      closest() { return this.label; },
      querySelector() { return this.children[0] ?? null; },
      replaceChildren(...children) {
        textContent = "";
        this.children = [];
        this.append(...children);
      },
      append(...children) {
        for (const child of children) { child.parentElement = this; this.children.push(child); }
      },
      remove() {
        if (this.parentElement) {
          const siblings = this.parentElement.children;
          siblings.splice(siblings.indexOf(this), 1);
          this.parentElement = null;
        }
      },
      getBoundingClientRect() { return { height: 20 }; },
      addEventListener(name, callback) {
        if (!events.has(name)) events.set(name, []);
        events.get(name).push(callback);
        if (selector) handlers.set(`${selector}:${name}`, event => this.fire(name, event));
      },
      dispatchEvent(event) {
        for (const callback of events.get(event.type) ?? []) callback(event);
        return !event.defaultPrevented;
      },
      fire(name, event = {}) {
        event.type ??= name;
        event.preventDefault ??= () => { event.defaultPrevented = true; };
        const results = (events.get(name) ?? []).map(callback => callback(event));
        return Promise.all(results);
      },
      focus() { document.activeElement = this; this.dispatchEvent({ type: "focus" }); },
      showModal() { this.open = true; },
      close() { if (this.open) { this.open = false; this.dispatchEvent({ type: "close" }); } },
    };
    element.label = { hidden: false, toggleAttribute(name, value) { this[name] = value; } };
    return element;
  };
  const el = selector => {
    if (!elements.has(selector)) elements.set(selector, node(selector));
    return elements.get(selector);
  };
  document = {
    hidden: false, activeElement: null, querySelector: el,
    createElement: tag => { const element = node(); element.tagName = tag.toUpperCase(); return element; },
    createTextNode: text => ({ textContent: text }),
    addEventListener(name, callback) {
      if (!listeners.has(name)) listeners.set(name, []);
      listeners.get(name).push(callback);
      handlers.set(`document:${name}`, event => Promise.all(listeners.get(name).map(callback => callback(event))));
    },
  };
  return { document, elements, handlers, el };
}
