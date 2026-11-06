import { deriveOperationEligibility } from "../../state/selectors.mjs";
import { queryElement, setText } from "../dom.mjs";

export function deriveSelfTestViewModel(state, sizeMiB) {
  const status = state.self_test;
  const speed = value => value == null ? "--" : `${(value / 1048576).toFixed(2)} MiB/s`;
  const count = value => `${(value / 1048576).toFixed(1)} MiB`;
  const labels = { idle: "Idle", queued: "Queued", preparing: "Preparing", writing: "Writing", reading: "Verifying",
    cleanup: "Cleanup", passed: "Passed", failed: "Failed", cancelled: "Cancelled" };
  const errors = ["", "Storage busy", "USB host owns the card", "Invalid test size", "Card unavailable",
    "Insufficient free space", "PFTEST.BIN already exists", "Filesystem I/O failed", "Readback verification failed",
    "Cleanup failed", "Filesystem not mounted"];
  const notices = [];
  if (status?.error) {
    notices.push(`${errors[status.error] ?? `Unknown error (${status.error})`} (FatFs ${status.filesystem_result}).`);
  }
  if (status && !status.valid_for_control) notices.push("Unknown status values; controls disabled.");
  if (status?.file_remains) notices.push("PFTEST.BIN remains on the card.");
  if (status?.mismatch_count) notices.push(`${status.mismatch_count} mismatched bytes.`);
  const ready = (state.eligibility ?? deriveOperationEligibility(state)).selfTestReady;
  const validSize = Number.isInteger(sizeMiB) && sizeMiB >= 1 && sizeMiB <= 1024;
  return {
    progress: status?.progress_percent ?? 0,
    result: ["passed", "failed"].includes(status?.state) ? status.state : "",
    autoOpen: status?.active ? "active" : status?.state === "failed" ? "failed" : "",
    startDisabled: !ready || status?.active || !state.memory?.initialized || !status?.volume_mounted || !validSize,
    cancelDisabled: !ready || !status?.active || status?.state === "cleanup",
    sizeDisabled: !ready || Boolean(status?.active),
    text: {
      "self-test-state": status
        ? `${labels[status.state] ?? status.state} · ${Math.floor(status.progress_percent ?? 0)}%` : "Unavailable",
      "self-test-write-speed": speed(status?.write_bytes_per_second),
      "self-test-read-speed": speed(status?.read_bytes_per_second),
      "self-test-written": status ? `${count(status.written_bytes)} / ${count(status.target_bytes)}` : "--",
      "self-test-read": status ? `${count(status.read_bytes)} / ${count(status.target_bytes)}` : "--",
      "self-test-write-time": status ? `${(status.write_elapsed_ms / 1000).toFixed(2)} s` : "--",
      "self-test-read-time": status ? `${(status.read_elapsed_ms / 1000).toFixed(2)} s` : "--",
      "self-test-message": notices.join(" "),
    },
  };
}

export function renderSelfTest(state) {
  const model = deriveSelfTestViewModel(state, queryElement("#self-test-size").valueAsNumber);
  setText(model.text);
  queryElement("#self-test-progress").value = model.progress;
  queryElement("#self-test-meter").dataset.result = model.result;
  const disclosure = queryElement("#self-test-disclosure");
  if (model.autoOpen && disclosure.dataset.autoOpen !== model.autoOpen) disclosure.open = true;
  disclosure.dataset.autoOpen = model.autoOpen;
  queryElement("#self-test-start").disabled = model.startDisabled;
  queryElement("#self-test-cancel").disabled = model.cancelDisabled;
  queryElement("#self-test-size").disabled = model.sizeDisabled;
}
