// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

// @ts-check
/**
 * @typedef {{session: string}} ConsoleSession
 * @typedef {{data: string, closed: boolean}} ConsoleRead
 * @typedef {{ok: true}} ConsoleAction
 * @typedef {"GET" | "POST" | "PUT" | "DELETE"} ConsoleMethod
 * @typedef {ConsoleSession | ConsoleRead | ConsoleAction} ConsoleResponse
 */

/** @param {unknown} result @param {ConsoleMethod} method @returns {ConsoleResponse} */
export function validateConsoleResponse(result, method) {
  if (result && typeof result === "object") {
    if (method === "POST" && "session" in result && typeof result.session === "string" && result.session.length > 0) {
      return { session: result.session };
    }
    if (method === "GET" && "data" in result && typeof result.data === "string"
      && /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(result.data)
      && "closed" in result && typeof result.closed === "boolean") {
      return { data: result.data, closed: result.closed };
    }
    if ((method === "PUT" || method === "DELETE") && "ok" in result && result.ok === true) return { ok: true };
  }
  throw new Error(`Invalid console ${method} response`);
}

/**
 * @overload
 * @param {string} url
 * @param {"POST"} method
 * @param {unknown} [payload]
 * @param {AbortSignal} [signal]
 * @param {number} [timeoutMs]
 * @returns {Promise<ConsoleSession>}
 */
/**
 * @overload
 * @param {string} url
 * @param {"GET"} [method]
 * @param {unknown} [payload]
 * @param {AbortSignal} [signal]
 * @param {number} [timeoutMs]
 * @returns {Promise<ConsoleRead>}
 */
/**
 * @overload
 * @param {string} url
 * @param {"PUT" | "DELETE"} method
 * @param {unknown} [payload]
 * @param {AbortSignal} [signal]
 * @param {number} [timeoutMs]
 * @returns {Promise<ConsoleAction>}
 */
/**
 * @param {string} url
 * @param {ConsoleMethod} [method]
 * @param {unknown} [payload]
 * @param {AbortSignal} [signal]
 * @param {number} [timeoutMs]
 * @returns {Promise<ConsoleResponse>}
 */
export async function consoleRequest(url, method = "GET", payload, signal, timeoutMs = 10000) {
  const controller = new AbortController();
  const timeout = setTimeout(() => {
    controller.abort(new DOMException("Console request timed out", "TimeoutError"));
  }, timeoutMs);
  const abort = () => controller.abort(signal?.reason);
  if (signal?.aborted) abort();
  else signal?.addEventListener("abort", abort, { once: true });

  try {
    const response = await fetch(url, {
      method, cache: "no-store", signal: controller.signal,
      ...(payload === undefined ? {} : {
        headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
      }),
    });
    /** @type {unknown} */
    const result = await response.json();
    if (!response.ok) {
      const error = result && typeof result === "object"
        ? "error" in result ? result.error : "detail" in result ? result.detail : undefined : undefined;
      throw new Error(typeof error === "string" ? error : `HTTP ${response.status}`);
    }
    return validateConsoleResponse(result, method);
  } catch (error) {
    if (controller.signal.aborted && controller.signal.reason?.name === "TimeoutError") {
      throw controller.signal.reason;
    }
    throw error;
  } finally {
    clearTimeout(timeout);
    signal?.removeEventListener("abort", abort);
  }
}
