/** Cache required DOM elements per document; no board or protocol access. */
const elementsByRoot = new WeakMap();

export function queryElement(selector, root = document) {
  let elements = elementsByRoot.get(root);
  if (!elements) elementsByRoot.set(root, elements = new Map());
  if (elements.has(selector)) return elements.get(selector);
  const element = root.querySelector(selector);
  if (!element) throw new Error(`Required dashboard element not found: ${selector}`);
  elements.set(selector, element);
  return element;
}

export function setText(values) {
  for (const [id, value] of Object.entries(values)) {
    const element = queryElement(`#${id}`);
    const text = value == null ? "" : String(value);
    if (element.textContent !== text) element.textContent = text;
  }
}
