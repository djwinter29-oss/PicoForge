import { bindWirelessForm } from "./wireless-form.mjs";

let snapshot = null;
let board = null;
let loadedAt = 0;
const url = () => `/api/wireless${board ? `?board=${encodeURIComponent(board)}` : ""}`;
const request = async options => {
  const response = await fetch(url(), { cache: "no-store", ...options });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `Wireless endpoint returned HTTP ${response.status}`);
  return result.wireless;
};
const ready = () => snapshot?.connected && !snapshot.settings_busy && !snapshot.outcome_uncertain &&
  !snapshot.reboot_pending && snapshot.msc && !snapshot.msc.enabled && !snapshot.msc.transition_pending &&
  !snapshot.msc_command_pending && !snapshot.self_test?.active && !snapshot.self_test_command_pending &&
  !snapshot.storage_stale && !snapshot.self_test_stale;
const form = bindWirelessForm({
  supported: () => Boolean(snapshot?.hardware?.capabilities & 128),
  ready,
  load: () => request(),
  async save(config) {
    let reboot = false;
    document.dispatchEvent(new CustomEvent("picoforge-wireless-operation", { detail: { busy: true } }));
    try {
      const saved = await request({ method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify(config) });
      reboot = true;
      return saved;
    } finally {
      document.dispatchEvent(new CustomEvent("picoforge-wireless-operation", {
        detail: { busy: false, reboot_pending: reboot },
      }));
    }
  },
});
document.addEventListener("picoforge-status", event => {
  const selected = event.detail.board;
  if (selected !== board || !event.detail.snapshot?.connected) {
    loadedAt = 0;
    form.reset();
  }
  board = selected;
  snapshot = event.detail.snapshot;
  form.update();
  if (Date.now() - loadedAt >= 2000 && ready() && (snapshot.hardware?.capabilities & 128)) {
    loadedAt = Date.now();
    void form.load();
  }
});
