// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

import { REASON } from "./state/errors.mjs";
// @ts-check
/**
 * @file Page-owned timer scopes so connection replacement and page teardown cancel scheduled work.
 */

/** @typedef {ReturnType<typeof globalThis.setTimeout> & {unref?: () => void}} TimerHandle */

export function createTimerScope(parent = null) {
  const timeouts = new Set();
  const intervals = new Set();
  const children = new Set();
  let disposed = false;
  const scope = {
    /** @return {TimerHandle} Browser timer ID, or an unref-capable handle in host tests. */
    setTimeout(callback, delay, ...args) {
      if (disposed) throw new Error("Timer scope is disposed");
      let handle;
      handle = globalThis.setTimeout(() => {
        timeouts.delete(handle);
        if (!disposed) callback(...args);
      }, delay);
      timeouts.add(handle);
      return handle;
    },
    /** @return {TimerHandle} */
    setInterval(callback, delay, ...args) {
      if (disposed) throw new Error("Timer scope is disposed");
      const handle = globalThis.setInterval(() => {
        if (!disposed) callback(...args);
      }, delay);
      intervals.add(handle);
      return handle;
    },
    clearTimeout(handle) {
      globalThis.clearTimeout(handle);
      timeouts.delete(handle);
    },
    clearInterval(handle) {
      globalThis.clearInterval(handle);
      intervals.delete(handle);
    },
    createChild() {
      if (disposed) throw new Error("Timer scope is disposed");
      const child = createTimerScope(scope);
      children.add(child);
      return child;
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      for (const handle of timeouts) globalThis.clearTimeout(handle);
      for (const handle of intervals) globalThis.clearInterval(handle);
      timeouts.clear();
      intervals.clear();
      for (const child of children) child.dispose();
      children.clear();
      parent?.removeChild(scope);
    },
    removeChild(child) { children.delete(child); },
  };
  return scope;
}

/** @param {Pick<typeof globalThis, "setTimeout" | "clearTimeout">} [timers] */
export function wait(milliseconds, signal, timers = globalThis) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(signal.reason ?? new DOMException(REASON.OPERATION_CANCELLED, "AbortError"));
      return;
    }
    const timer = timers.setTimeout(finish, milliseconds);
    function finish() {
      signal?.removeEventListener("abort", abort);
      resolve();
    }
    function abort() {
      timers.clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      reject(signal.reason ?? new DOMException(REASON.OPERATION_CANCELLED, "AbortError"));
    }
    signal?.addEventListener("abort", abort, { once: true });
  });
}

/** @param {Pick<typeof globalThis, "setTimeout" | "clearTimeout">} [timers] */
export function withTimeout(operation, milliseconds, signal, timers = globalThis) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (callback, value) => {
      if (settled) return;
      settled = true;
      timers.clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      callback(value);
    };
    const abort = () => finish(reject, signal.reason ?? new DOMException(REASON.OPERATION_CANCELLED, "AbortError"));
    const timer = timers.setTimeout(() => finish(reject,
      new DOMException(REASON.IO_TIMEOUT, "TimeoutError")), milliseconds);
    if (signal?.aborted) abort();
    else signal?.addEventListener("abort", abort, { once: true });
    Promise.resolve(operation).then(value => finish(resolve, value), error => finish(reject, error));
  });
}
