"""Bounded bridge from the local dashboard to the board's raw TCP CLI."""

from __future__ import annotations

import ipaddress
import logging
import secrets
import select
import socket
import threading
import time
from collections.abc import Callable

from ..reports.common import ProtocolError

_LOGGER = logging.getLogger(__name__)
# ponytail: one long-poll GET per 2 s idle replaces 20 polls/s; a WebSocket would only cut the per-chunk request.
_READ_WAIT_SECONDS = 2.0


class ConsoleBridge:
    """Relay raw bytes for one board session; close it after 15 seconds without a request."""

    def __init__(self):
        self._lock = threading.Lock()
        self._socket = None
        self._session = None
        self._board_id = None
        self._connecting = False
        self._generation = 0
        self._last_request = 0

    @staticmethod
    def _authenticate(connection, sign_proof: Callable[[bytes, bytes], bytes]):
        connection.settimeout(3)
        challenge = bytearray()
        while len(challenge) < 128 and not challenge.endswith(b"\r\n"):
            chunk = connection.recv(1)
            if not chunk:
                raise OSError("Board closed the TCP CLI during authentication")
            challenge.extend(chunk)
        if not challenge.endswith(b"\r\n"):
            raise ValueError("Invalid TCP CLI authentication challenge")
        try:
            tag, kind, nonce_hex, uid_hex = challenge[:-2].decode("ascii").split(" ")
            nonce, uid = bytes.fromhex(nonce_hex), bytes.fromhex(uid_hex)
        except ValueError as error:
            raise ProtocolError("Invalid TCP CLI authentication challenge") from error
        # Text and byte lengths together reject the whitespace bytes.fromhex() would otherwise skip.
        if ((tag, kind) != ("PicoForge", "AUTH") or len(nonce_hex) != 32 or len(nonce) != 16
                or len(uid_hex) != 16 or len(uid) != 8):
            raise ProtocolError("Invalid TCP CLI authentication challenge")
        proof = sign_proof(nonce, uid)
        if not isinstance(proof, bytes) or len(proof) != 16:
            raise RuntimeError("The connected board returned an invalid TCP CLI proof")
        connection.sendall(b"AUTH " + proof.hex().encode("ascii") + b"\r\n")

    def open(self, wireless, sign_proof: Callable[[bytes, bytes], bytes] | None = None,
             board_id: str | None = None):
        if wireless.get("stale"):
            raise RuntimeError("The board's wireless status is stale; wait for the next refresh")
        if not (wireless.get("wifi_enabled") and wireless.get("wifi_state") == "connected"
                and wireless.get("tcp_cli_started")):
            raise RuntimeError("The board's Wi-Fi TCP CLI is not available")
        try:
            address = ipaddress.IPv4Address(wireless["ipv4"])
        except ipaddress.AddressValueError as error:
            raise ProtocolError("Invalid board IPv4 address") from error
        if address.is_loopback or address.is_multicast or address.is_unspecified or int(address) == 0xFFFFFFFF:
            raise ProtocolError("Invalid board IPv4 address")
        if wireless.get("ble_key_configured") and sign_proof is None:
            raise RuntimeError("USB Control key proof is unavailable")
        with self._lock:
            if self._socket is not None or self._connecting:
                raise RuntimeError("A web console is already connected; disconnect it first")
            self._connecting = True
            generation = self._generation
        connection = None
        installed = False
        try:
            connection = socket.create_connection((str(address), 2323), timeout=3)
            if wireless.get("ble_key_configured"):
                self._authenticate(connection, sign_proof)
            connection.settimeout(0.1)
            with self._lock:
                if generation != self._generation:
                    raise RuntimeError("Console connection was cancelled during dashboard shutdown")
                if self._socket is not None:
                    raise RuntimeError("A web console is already connected; disconnect it first")
                self._socket = connection
                self._session = secrets.token_urlsafe(32)
                self._board_id = board_id
                self._last_request = time.monotonic()
                self._schedule_expiry(15)
                installed = True
                return self._session
        finally:
            with self._lock:
                self._connecting = False
            if connection is not None and not installed:
                try:
                    connection.close()
                except OSError:
                    _LOGGER.exception("Failed to close an uninstalled TCP console connection")

    def _schedule_expiry(self, delay):
        timer = threading.Timer(delay, self._expire, args=(self._session,))
        timer.daemon = True
        timer.start()

    def _expire(self, session=None):
        with self._lock:
            if self._socket is None or session is not None and session != self._session:
                return
            remaining = 15 - (time.monotonic() - self._last_request)
            if remaining > 0:
                self._schedule_expiry(remaining)
            else:
                self._close()

    def _require_session(self, session):
        if self._socket is None or not secrets.compare_digest(self._session, session or ""):
            raise RuntimeError("Console disconnected; connect again")
        self._last_request = time.monotonic()
        return self._socket

    def _close(self):
        self._socket.close()
        self._socket = self._session = None
        self._board_id = None

    def read(self, session):
        """Wait up to 2 s for board output without holding the lock, so writes are never blocked."""
        with self._lock:
            connection = self._require_session(session)
        try:
            ready = select.select([connection], [], [], _READ_WAIT_SECONDS)[0]
        except (OSError, ValueError):
            ready = []
        with self._lock:
            if self._require_session(session) is not connection:
                raise RuntimeError("Console disconnected; connect again")
            if not ready:
                return b"", False
            try:
                data = connection.recv(4096)
            except socket.timeout:
                return b"", False
            except OSError:
                self._close()
                raise
            if not data:
                self._close()
            return data, not data

    def write(self, session, data):
        with self._lock:
            self._require_session(session)
            try:
                self._socket.sendall(data)
            except OSError:
                self._close()
                raise

    def close(self, session):
        with self._lock:
            self._require_session(session)
            self._close()

    def close_all(self) -> None:
        with self._lock:
            self._generation += 1
            if self._socket is not None:
                self._close()
