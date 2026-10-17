import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus, renderSelfTest } from "./view.mjs";

const connect = document.querySelector("#connect");
const disconnect = document.querySelector("#disconnect");
const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  connect.disabled = state.busy || state.connected || state.reconnecting || !navigator.hid;
  disconnect.disabled = state.busy || !state.connected;
  ledControl.disabled = state.busy || !state.connected || !state.board_status?.led_available;
  mscControl.disabled = state.busy || !state.connected || !state.msc || Boolean(state.msc.transition_pending)
    || Boolean(state.self_test?.active) || state.self_test_command_pending;
  mscControl.textContent = state.msc?.enabled ? "Disable USB drive" : "Enable USB drive";
  document.querySelector("#msc-note").textContent = state.reconnecting
    ? "USB interfaces are reconnecting; HID will resume automatically."
    : state.msc?.request_rejected
    ? "Request refused. Safely eject the drive on the host before disabling."
    : state.msc?.transition_pending ? "USB mode changing; HID reconnects automatically."
      : state.msc?.enabled ? "Host owns the card. Safely eject before disabling to return ownership to Pico."
        : "Pico owns the card. Unmount firmware storage before enabling the USB drive.";
}

connect.addEventListener("click", () => client.connect());
disconnect.addEventListener("click", () => client.disconnect());
ledControl.addEventListener("click", () => client.setLed(!client.state.board_status?.led_on));
mscControl.addEventListener("click", () => client.setMsc(!client.state.msc?.enabled));
document.querySelector("#self-test-start").addEventListener("click", async () => {
  await client.setSelfTest("start", document.querySelector("#self-test-size").valueAsNumber);
  render(client.state);
});
document.querySelector("#self-test-cancel").addEventListener("click", () => client.setSelfTest("cancel"));
document.querySelector("#self-test-size").addEventListener("input", () => renderSelfTest(client.state));
render(client.state);