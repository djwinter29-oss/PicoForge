export async function consoleRequest(url, method = "GET", payload, signal, timeoutMs = 10000) {
  const controller = new AbortController();
  const timeout = setTimeout(() => {
    controller.abort(new DOMException("Console request timed out", "TimeoutError"));
  }, timeoutMs);
  const abort = () => controller.abort(signal.reason);
  if (signal?.aborted) abort();
  else signal?.addEventListener("abort", abort, { once: true });

  try {
    const response = await fetch(url, {
      method, cache: "no-store", signal: controller.signal,
      ...(payload === undefined ? {} : {
        headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
      }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error ?? result.detail ?? `HTTP ${response.status}`);
    return result;
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
