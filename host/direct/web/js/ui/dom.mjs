// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/** Cache required DOM elements per document; no board or protocol access. */
/** @type {WeakMap<Document, Map<string, HTMLElement>>} */
const elementsByRoot = new WeakMap();

/**
 * @template {keyof HTMLElementTagNameMap} K
 * @overload
 * @param {`${K}#${string}`} selector
 * @param {Document} [root]
 * @return {HTMLElementTagNameMap[K]}
 */
/**
 * @overload
 * @param {string} selector
 * @param {Document} [root]
 * @return {HTMLElement}
 */
/**
 * @param {string} selector
 * @param {Document} [root]
 * @return {HTMLElement}
 */
export function queryElement(selector, root = document) {
  let elements = elementsByRoot.get(root);
  if (!elements) elementsByRoot.set(root, elements = new Map());
  if (elements.has(selector)) return elements.get(selector);
  const element = root.querySelector(selector);
  if (!element) throw new Error(`Required dashboard element not found: ${selector}`);
  if (element.namespaceURI !== "http://www.w3.org/1999/xhtml") {
    throw new Error(`Required dashboard element is not HTML: ${selector}`);
  }
  const tag = selector.match(/^([a-z][a-z0-9]*)#/i)?.[1].toLowerCase();
  if (tag && element.localName !== tag) {
    throw new Error(`Required dashboard element has the wrong tag: ${selector}`);
  }
  // The namespace and optional tag checks validate the typed lookup, including compound CSS selectors.
  const html = /** @type {HTMLElement} */ (element);
  elements.set(selector, html);
  return html;
}

/** @param {Record<string, unknown>} values */
export function setText(values) {
  for (const [id, value] of Object.entries(values)) {
    const element = queryElement(`#${id}`);
    const text = value == null ? "" : String(value);
    if (element.textContent !== text) element.textContent = text;
  }
}
