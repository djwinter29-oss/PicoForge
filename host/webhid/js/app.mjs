import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus } from "./view.mjs";

const connect = document.querySelector("#connect");
const disconnect = document.querySelector("#disconnect");
const ledControl = document.querySelector("#led-control");
const mscControl = document.querySelector("#msc-control");
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  connect.disabled = state.busy || state.connected || !navigator.hid;
  disconnect.disabled = state.busy || !state.connected;
  ledControl.disabled = state.busy || !state.connected || !state.board_status?.led_available;
  mscControl.disabled = state.busy || !state.connected || !state.msc || Boolean(state.msc.transition_pending);
  mscControl.textContent = state.msc?.enabled ? "Disable USB drive" : "Enable USB drive";
  document.querySelector("#msc-note").textContent = state.msc?.request_rejected
    ? "Request refused. Safely eject the drive on the host before disabling."
    : state.msc?.transition_pending ? "USB mode changing; reconnect to continue."
      : state.msc?.enabled ? "Host owns the card. Safely eject before disabling or using it in firmware."
        : "Firmware owns the card. Unmount firmware storage before enabling; the device then reconnects.";
}

connect.addEventListener("click", () => client.connect());
disconnect.addEventListener("click", () => client.disconnect());
ledControl.addEventListener("click", () => client.setLed(!client.state.board_status?.led_on));
mscControl.addEventListener("click", () => client.setMsc(!client.state.msc?.enabled));
render(client.state);