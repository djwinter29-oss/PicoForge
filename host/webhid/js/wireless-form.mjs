/** Bind the private USB provisioning fields; Board configuration saves them with its settings. */
export function bindWirelessForm({ load, ready, supported, changed = () => {} }) {
  const el = id => document.querySelector(`#${id}`);
  const panel = el("wireless-panel");
  if (!panel) return { dirty: () => false, request() {}, finish() {}, reset() {}, update() {}, async load() {} };
  let loading = false;
  let dirty = false;
  let status = null;
  let generation = 0;
  let message = "";
  const fields = ["wifi-ssid", "wifi-password", "ble-secret"];
  const refresh = () => {
    panel.hidden = !supported();
    for (const id of fields) el(id).disabled = !ready();
    el("wireless-generate").disabled = !ready();
    el("wireless-status").textContent = message || (status
      ? `Wi-Fi: ${status.wifi_state}, IP: ${status.ipv4 || "--"}. `
        + `BLE key: ${status.ble_enabled ? "configured" : "none"}.`
      : "Connect over USB to configure wireless.");
  };
  const edited = () => { dirty = true; changed(); refresh(); };
  for (const id of fields) el(id).addEventListener("input", () => { message = ""; edited(); });
  el("wireless-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("ble-secret").value = [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
    bytes.fill(0);
    el("ble-secret").type = "text";
    message = "Copy the generated BLE key to a password manager before saving; the board never returns it.";
    edited();
  });
  const clearSecrets = () => {
    el("wifi-password").value = "";
    el("ble-secret").value = "";
    el("ble-secret").type = "password";
  };
  return {
    dirty: () => dirty,
    /** Credential part of a combined save; Board configuration adds the board settings. */
    request() {
      // Credentials provision a radio; the Wi-Fi/Bluetooth switches decide whether it starts.
      const config = { wifi_enabled: Boolean(el("wifi-ssid").value),
        ble_enabled: Boolean(el("ble-secret").value || status?.ble_enabled), ssid: el("wifi-ssid").value,
        confirm_reboot: true };
      if (el("wifi-password").value) config.password = el("wifi-password").value;
      if (el("ble-secret").value) config.ble_key = el("ble-secret").value;
      return config;
    },
    /** Clear secrets after any save attempt; keep edits only on failure. */
    finish(saved, error) {
      clearSecrets();
      if (saved) { status = saved; dirty = false; message = "Wireless settings saved."; }
      else message = error;
      refresh();
    },
    reset() {
      generation++;
      status = null;
      dirty = false;
      message = "";
      el("wifi-ssid").value = "";
      clearSecrets();
    },
    update: refresh,
    async load() {
      if (loading || !supported() || !ready()) return;
      loading = true;
      const current = generation;
      try {
        const fresh = await load();
        if (generation !== current) return;
        status = fresh;
        if (!dirty) el("wifi-ssid").value = status.ssid;
      } catch (error) { if (generation === current) message = error.message; }
      finally { loading = false; refresh(); }
    },
  };
}
