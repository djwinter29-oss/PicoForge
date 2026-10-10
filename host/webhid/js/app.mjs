import { renderStatus } from "../../web/js/dashboard.mjs";
import { PicoForgeWebHid } from "./device.mjs";

const connect = document.querySelector("#connect");
const refresh = document.querySelector("#refresh");
const disconnect = document.querySelector("#disconnect");
const client = new PicoForgeWebHid(globalThis.isSecureContext ? navigator.hid : undefined, render);

function render(state) {
  renderStatus(state);
  connect.disabled = state.busy || state.connected || !navigator.hid;
  refresh.disabled = state.busy || !state.connected;
  disconnect.disabled = state.busy || !state.connected;
}

connect.addEventListener("click", () => client.connect());
refresh.addEventListener("click", () => client.refresh());
disconnect.addEventListener("click", () => client.disconnect());
render(client.state);