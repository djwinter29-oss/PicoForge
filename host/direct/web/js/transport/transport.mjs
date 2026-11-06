// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Ji Dong

/**
 * Browser-link contract consumed by the board session. A transport is a byte pipe: framing checks only, no protocol
 * policy (link restrictions live in protocol/opcodes.mjs). `device` is an opaque connection identity the session
 * compares for fencing; it is never dereferenced outside the transport. Events are `report` (reportId, data),
 * `disconnect`, `available` (an authorized reconnect candidate), and `suspend`. Every I/O operation receives the
 * connection signal.
 *
 * @typedef {EventTarget & {
 *   kind: "usb" | "ble", available: boolean, unavailableReason: string, autoReconnect: boolean,
 *   pollInterval: number | null, device: object | null, name: string | null,
 *   connect: (options: Uint8Array | undefined, signal: AbortSignal) => Promise<boolean>,
 *   reconnect: (signal: AbortSignal) => Promise<boolean>,
 *   disconnect: () => Promise<void>, dispose: () => Promise<void>,
 *   readReport: (id: number, signal: AbortSignal) => Promise<DataView | Uint8Array>,
 *   writeReport: (id: number, data: Uint8Array, signal: AbortSignal) => Promise<void>,
 *   createConsoleProof: (nonce: Uint8Array, uid: Uint8Array, signal: AbortSignal) => Promise<Uint8Array | null>
 * }} Transport
 */
export {};
