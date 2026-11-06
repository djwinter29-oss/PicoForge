/**
 * @file Pure Feature 12 interpretation shared by the session (polling, recovery) and wireless workflows.
 */
import { OP, SCAN_STATE, STAGED_EXPIRED, WIFI_STATE } from "../protocol/constants.mjs";

/** @brief Feature 12 poll interval: fast while a Wi-Fi test or scan runs, otherwise 2 s. */
export const wirelessPollInterval = wireless => wireless?.state === WIFI_STATE.TESTING
  || wireless?.scan_state === SCAN_STATE.SCANNING ? 500 : 2000;

/** @brief Whether a snapshot shows the saved (not staged or under test) SSID. */
export const showsSavedSsid = wireless => !wireless.staged_mask
  && ![WIFI_STATE.TESTING, WIFI_STATE.TEST_JOINED, WIFI_STATE.TEST_FAILED].includes(wireless.state);

/**
 * @brief Interpret one Feature 12 snapshot for an accepted scan/test lifecycle.
 * @param status Decoded Feature 12 snapshot.
 * @param pending Lifecycle record `{operation, generation}`.
 * @return `{kind: "completed", result}`, `{kind: "failed", message}`, or null while still running; throws when the
 *   board started a different generation.
 */
export function lifecycleOutcome(status, pending) {
  if (pending.operation === OP.SCAN) {
    if (status.scan_generation !== pending.generation) throw new Error("Wireless scan generation changed");
    if (status.scan_state === SCAN_STATE.FAILED) return { kind: "failed", message: "Wireless scan failed" };
    if (status.scan_state === SCAN_STATE.COMPLETE) return { kind: "completed", result: status };
  } else {
    if (status.test_generation !== pending.generation) throw new Error("Wireless test generation changed");
    if (status.state === WIFI_STATE.TEST_FAILED || status.staged_state & STAGED_EXPIRED) {
      return { kind: "failed", message: status.phase || "Test credentials expired" };
    }
    if (status.state === WIFI_STATE.TEST_JOINED && status.pair_tested) {
      // ponytail: no lifetime is transmitted; the board starts a 600s window on success. Subtract polling latency.
      return { kind: "completed", result: { ...status, remaining_seconds: 599 } };
    }
  }
  return null;
}
