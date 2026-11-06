import { createDashboardStore, createUiStore } from "../web/js/state/store.mjs";

const board = createDashboardStore();
board.dispatch({ type: "command/started", changes: { busy: true, pending_request: { transaction_id: 1, opcode: 1 } } });
// @ts-expect-error Busy is a boolean, not display text.
board.dispatch({ type: "command/started", changes: { busy: "yes" } });
// @ts-expect-error Transaction IDs are numbers.
board.dispatch({ type: "command/started", changes: { pending_request: { transaction_id: "1", opcode: 1 } } });
// @ts-expect-error Freshness uses the declared states.
board.dispatch({ type: "report/received", changes: { freshness: { 3: "recent" } } });
// @ts-expect-error Presentation state does not belong in domain state.
board.dispatch({ type: "connection/changed", changes: { settings_dirty: true } });

const ui = createUiStore();
ui.dispatch({ type: "patch", changes: { settings_dirty: true } });
// @ts-expect-error UI flags are booleans.
ui.dispatch({ type: "patch", changes: { settings_dirty: 1 } });
// @ts-expect-error Domain state does not belong in the UI store.
ui.dispatch({ type: "patch", changes: { connected: true } });
