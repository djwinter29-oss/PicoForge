import {
  bytesToHex, CONSOLE_AUTH_ERROR, CONSOLE_AUTH_REQUEST, CONSOLE_AUTH_RESPONSE, parseHexBytes,
} from "../console-auth.mjs";

/**
 * @brief Open the Wi-Fi console and answer its proof requests for the current session.
 * @param getSession Returns the active board session.
 * @param reportError Shows a UI-only failure; the console link never writes domain state.
 */
export function bindConsoleLink(getSession, reportError) {
  const windows = new WeakSet();
  async function provideProof(event) {
    const request = event.data;
    const popup = event.source;
    if (event.origin !== location.origin || !popup || !windows.has(popup)
      || request?.type !== CONSOLE_AUTH_REQUEST || typeof request.request_id !== "string"
      || !/^[a-f0-9]{32}$/i.test(request.request_id)) return;
    windows.delete(popup);
    const response = { request_id: request.request_id };
    try {
      const nonce = parseHexBytes(request.nonce, 16, "TCP CLI nonce");
      const uid = parseHexBytes(request.uid, 8, "TCP CLI board identity");
      const session = getSession();
      if (!session.state.connected || !session.state.hardware) {
        throw new Error("Connect this board to the Direct dashboard over USB or Bluetooth first");
      }
      const proof = await session.authorizeConsole(nonce, uid);
      if (session !== getSession() || !session.state.connected) {
        throw new Error("The dashboard disconnected while authorizing the Wi-Fi console");
      }
      if (!(proof instanceof Uint8Array) || proof.length !== 16) {
        throw new Error("The board returned an invalid TCP CLI proof");
      }
      popup.postMessage({ ...response, type: CONSOLE_AUTH_RESPONSE,
        uid: request.uid.toLowerCase(), proof: bytesToHex(proof) }, event.origin);
    } catch (error) {
      popup.postMessage({ ...response, type: CONSOLE_AUTH_ERROR,
        error: error instanceof Error ? error.message : "Could not authorize the Wi-Fi console" }, event.origin);
    }
  }
  const onMessage = event => { void provideProof(event); };
  window.addEventListener?.("message", onMessage);
  return {
    open(event, href) {
      event.preventDefault();
      const url = new URL(href, location.href);
      if (url.origin !== location.origin || url.pathname !== new URL("console.html", location.href).pathname) {
        reportError("The Wi-Fi console URL is not part of this dashboard.");
        return;
      }
      const popup = window.open(url.href, "_blank");
      if (!popup) {
        reportError("The browser blocked the Wi-Fi console popup; allow popups for this dashboard.");
        return;
      }
      windows.add(popup);
    },
    dispose() { window.removeEventListener?.("message", onMessage); },
  };
}
