/**
 * @file Same-board reconnect recovery: read the result of the journalled request or observe the accepted wireless
 * lifecycle, never replaying either. Settles a waiting forced save with whatever it learns.
 */
import { OP, RESULT } from "../protocol/constants.mjs";
import { BoardError, errorValue, REASON } from "../state/errors.mjs";
import { commandFailed } from "./commands.mjs";

export class Recovery {
  /** @param {import("./session.mjs").BoardSession} session */
  constructor(session) {
    this.session = session;
  }

  /**
   * @brief Recover every pending fact after the board identified itself; failures leave the outcome uncertain.
   * @param device Device the connection was opened on.
   * @param {AbortSignal} signal Connection signal.
   * @param {() => boolean} current Whether that connection is still the active one.
   */
  async run(device, signal, current) {
    const s = this.session;
    const request = s.state.pending_request;
    if (request) await this.recoverRequest(device, request, signal);
    const pending = s.state.pending_wireless;
    if (pending && !s.state.pending_request) await this.recoverLifecycle(pending, current);
  }

  /** @brief Step 1: read the journalled control result and retire it, or keep it uncertain. */
  async recoverRequest(device, request, signal) {
    const s = this.session;
    const commands = s.commands;
    try {
      const result = await commands.pollResult(device, request, signal);
      const succeeded = result.status === RESULT.COMPLETED;
      const unconfirmed = succeeded && commands.unconfirmed(request, result);
      if (unconfirmed) {
        throw new BoardError(unconfirmed === REASON.NOT_CONFIRMED ? REASON.RECOVERY_NOT_CONFIRMED : unconfirmed);
      }
      commands.retire(request, succeeded);
      s.emit("recovery/finished", { outcome_uncertain: false,
        error: succeeded ? null : errorValue(commandFailed(result, false)),
        recovered_opcode: succeeded ? result.opcode : null,
        recovery_message: !succeeded ? null
          : result.opcode === OP.CANCEL_TEST ? REASON.RECOVERED_CANCEL : REASON.RECOVERED_COMMAND });
      commands.settleForcedSave({ result });
    } catch (error) {
      s.emit("recovery/finished", { outcome_uncertain: true, error: errorValue(error) });
      commands.settleForcedSave({ error });
    }
  }

  /** @brief Step 2: observe the accepted scan/test lifecycle to its outcome and clear its journal record. */
  async recoverLifecycle(pending, current) {
    const s = this.session;
    try {
      const outcome = await s.polling.waitLifecycle(pending);
      if (!current()) throw new BoardError(REASON.WIRELESS_UNCERTAIN);
      s.commands.settleWireless();
      const recoveryError = s.commands.recoveryError;
      s.emit("recovery/finished", { outcome_uncertain: Boolean(recoveryError),
        error: recoveryError ?? (outcome.kind === "failed" ? errorValue(outcome.error) : null),
        recovery_message: outcome.kind === "completed" ? REASON.RECOVERED_WIRELESS : null });
    } catch (error) {
      s.emit("recovery/finished", { outcome_uncertain: true, error: errorValue(error) });
    }
  }
}
