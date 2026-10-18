import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus, renderSelfTest } from "./view.mjs";

const connect = document.querySelector("#connect");
const disconnect = document.querySelector("#disconnect");
const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const mscForceReturn = document.querySelector("#msc-force-return");
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  connect.disabled = state.busy || state.connected || state.reconnecting || !navigator.hid;
  disconnect.disabled = state.busy || !state.connected;
  ledControl.disabled = state.busy || !state.connected || !state.board_status?.led_available;
  mscControl.disabled = state.busy || !state.connected || !state.msc || Boolean(state.msc.transition_pending)
    || Boolean(state.self_test?.active) || state.self_test_command_pending;
  mscControl.textContent = state.msc?.enabled ? "Disable USB drive" : "Enable USB drive";
  mscForceReturn.disabled = mscControl.disabled || !state.msc?.enabled || !state.msc?.force_return_supported;
  document.querySelector("#msc-note").textContent = state.reconnecting
    ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.request_rejected
    ? (state.msc.enabled ? "Request refused. Safely eject the drive on the host before disabling."
      : "Storage return failed. Volume may be unmounted; inspect media and UART errors before mounting.")
    : state.msc?.transition_pending ? "Preparing storage and changing USB mode; HID reconnects automatically."
      : state.msc?.enabled ? "Host owns the card. Safely eject before disabling to return ownership to Pico."
        : "Firmware owns storage. Enable USB drive to unmount it safely and reconnect as a USB disk.";
}

connect.addEventListener("click", () => client.connect());
disconnect.addEventListener("click", () => client.disconnect());
ledControl.addEventListener("click", () => client.setLed(!client.state.board_status?.led_on));
mscControl.addEventListener("click", () => client.setMsc(!client.state.msc?.enabled));
mscForceReturn.addEventListener("click", () => {
  if (window.confirm("Force return to Pico? Cached host writes may be lost and the filesystem may be corrupted. "
    + "Stop all host file activity first. Pico will attempt to remount without formatting or repair. Continue?")) {
    void client.setMsc(false, true).catch(error => client.update({ error: error.message }));
  }
});
document.querySelector("#self-test-start").addEventListener("click", async () => {
  await client.setSelfTest("start", document.querySelector("#self-test-size").valueAsNumber);
  render(client.state);
});
document.querySelector("#self-test-cancel").addEventListener("click", () => client.setSelfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(client.state));
render(client.state);