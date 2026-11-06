/** Direct-only DOM fixture with additive listeners and explicit asynchronous event dispatch. */
export function createDocument() {
  const elements = new Map();
  const makeElement = () => ({
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
    append(...children) { this.children.push(...children); },
    replaceChildren(...children) { this.children = children; },
    showModal() { this.open = true; },
    close() { this.open = false; },
    focus() { this.dispatchEvent(new Event("focus")); },
  });
  const element = selector => {
    if (!elements.has(selector)) elements.set(selector, makeElement());
    return elements.get(selector);
  };
  return {
    document: { querySelector: element, createElement: makeElement },
    element,
    async fire(selector, type) {
      const event = new Event(type, { cancelable: true });
      for (const handler of element(selector).listeners.get(type) ?? []) await handler(event);
    },
  };
}
