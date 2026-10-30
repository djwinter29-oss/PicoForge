/** Bind the same private USB provisioning form to native-host or WebHID operations. */
export function bindWirelessForm({ load, save, ready, supported }) {
  const el = id => document.querySelector(`#${id}`);
  const panel = el("wireless-panel");
  if (!panel) return { update() {} };
  let busy = false;
  let loading = false;
  let dirty = false;
  let status = null;
  let generation = 0;
  let message = "";
  const fields = ["wifi-enabled", "ble-enabled", "wifi-ssid", "wifi-password", "ble-secret"];
  const refresh = () => {
    panel.hidden = !supported();
    for (const id of fields) el(id).disabled = busy || !ready();
    el("wireless-save").disabled = busy || !ready() || !dirty;
    el("wireless-generate").disabled = busy || !ready();
    el("wireless-status").textContent = message || (status
      ? `Wi-Fi: ${status.wifi_state}, IP: ${status.ipv4 || "--"}. BLE: ${status.ble_enabled ? "enabled" : "disabled"}.`
      : "Connect over USB to configure wireless.");
  };
  for (const id of fields) el(id).addEventListener("input", () => { dirty = true; refresh(); });
  el("wireless-generate").addEventListener("click", () => {
    const bytes = crypto.getRandomValues(new Uint8Array(32));
    el("ble-secret").value = [...bytes].map(byte => byte.toString(16).padStart(2, "0")).join("");
    bytes.fill(0);
    el("ble-secret").type = "text";
    message = "Copy the generated BLE key to a password manager before saving; the board never returns it.";
    dirty = true;
    refresh();
  });
  el("wireless-save").addEventListener("click", () => {
    if (busy || !ready() || !dirty) return;
    el("wireless-dialog").showModal();
  });
  el("wireless-cancel").addEventListener("click", () => el("wireless-dialog").close());
  el("wireless-dialog").addEventListener("keydown", event => {
    if (event.key === "Escape") { event.preventDefault(); el("wireless-dialog").close(); }
  });
  el("wireless-confirm").addEventListener("click", async () => {
    if (!el("wireless-dialog").open) return;
    el("wireless-dialog").close();
    if (busy || !ready()) return;
    const config = { wifi_enabled: el("wifi-enabled").checked, ble_enabled: el("ble-enabled").checked,
      ssid: el("wifi-ssid").value, confirm_reboot: true };
    if (el("wifi-password").value) config.password = el("wifi-password").value;
    if (el("ble-secret").value) config.ble_key = el("ble-secret").value;
    busy = true;
    refresh();
    try {
      status = await save(config);
      dirty = false;
      message = "Wireless settings saved. Waiting for reboot.";
    } catch (error) { message = error.message; }
    finally {
      el("wifi-password").value = "";
      el("ble-secret").value = "";
      el("ble-secret").type = "password";
      delete config.password;
      delete config.ble_key;
      busy = false;
      refresh();
    }
  });
  return {
    reset() {
      generation++;
      status = null;
      dirty = false;
      message = "";
      for (const id of ["wifi-ssid", "wifi-password", "ble-secret"]) el(id).value = "";
      el("wifi-enabled").checked = el("ble-enabled").checked = false;
      el("ble-secret").type = "password";
      el("wireless-dialog").close();
    },
    update() { if (!ready()) el("wireless-dialog").close(); refresh(); },
    async load() {
      if (busy || loading || !supported() || !ready()) return;
      loading = true;
      const current = generation;
      try {
        const fresh = await load();
        if (generation !== current || busy) return;
        status = fresh;
        if (!dirty) {
          el("wifi-enabled").checked = status.wifi_enabled;
          el("ble-enabled").checked = status.ble_enabled;
          el("wifi-ssid").value = status.ssid;
        }
      } catch (error) { if (generation === current) message = error.message; }
      finally { loading = false; refresh(); }
    },
  };
}
