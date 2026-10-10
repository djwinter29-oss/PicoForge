import { PicoForgeWebHid } from "./device.mjs";
import { renderStatus } from "./view.mjs";

const connect = document.querySelector("#connect");
const refresh = document.querySelector("#refresh");
const disconnect = document.querySelector("#disconnect");
const ledControl = document.querySelector("#led-control");
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  connect.disabled = state.busy || state.connected || !navigator.hid;
  refresh.disabled = state.busy || !state.connected;
  disconnect.disabled = state.busy || !state.connected;
  ledControl.disabled = state.busy || !state.connected || !state.board_status?.led_available;
}

connect.addEventListener("click", () => client.connect());
refresh.addEventListener("click", () => client.refresh());
disconnect.addEventListener("click", () => client.disconnect());
ledControl.addEventListener("click", () => client.setLed(!client.state.board_status?.led_on));
render(client.state);