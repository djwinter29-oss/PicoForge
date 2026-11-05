"""Bounded bridge from the local dashboard to the board's raw TCP CLI."""

from __future__ import annotations

import ipaddress
import secrets
import socket
import threading
import time
from collections.abc import Callable


class ConsoleBridge:
    """Relay raw bytes; close abandoned tabs after 15 seconds without a request."""

    def __init__(self):
        self._lock = threading.Lock()
        self._socket = None
        self._session = None
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
            fields = challenge[:-2].decode("ascii").split(" ")
        except UnicodeDecodeError as error:
            raise ValueError("Invalid TCP CLI authentication challenge") from error
        if (len(fields) != 4 or fields[:2] != ["PicoForge", "AUTH"] or len(fields[2]) != 32
                or any(character not in "0123456789abcdefABCDEF" for character in fields[2])
                or len(fields[3]) != 16
                or any(character not in "0123456789abcdefABCDEF" for character in fields[3])):
            raise ValueError("Invalid TCP CLI authentication challenge")
        nonce = bytes.fromhex(fields[2])
        uid = bytes.fromhex(fields[3])
        proof = sign_proof(nonce, uid)
        if not isinstance(proof, bytes) or len(proof) != 16:
            raise RuntimeError("The connected board returned an invalid TCP CLI proof")
        connection.sendall(b"AUTH " + proof.hex().encode("ascii") + b"\r\n")

    def open(self, wireless, sign_proof: Callable[[bytes, bytes], bytes] | None = None):
        if not (wireless.get("wifi_enabled") and wireless.get("wifi_state") == "connected"
                and wireless.get("tcp_cli_started")):
            raise RuntimeError("The board's Wi-Fi TCP CLI is not available")
        address = ipaddress.IPv4Address(wireless["ipv4"])
        if address.is_loopback or address.is_multicast or address.is_unspecified or int(address) == 0xFFFFFFFF:
            raise ValueError("Invalid board IPv4 address")
        if wireless.get("tcp_cli_auth_required") and sign_proof is None:
            raise RuntimeError("USB Control key proof is unavailable")
        with self._lock:
            if self._socket is not None:
                raise RuntimeError("A web console is already connected; disconnect it first")
            connection = socket.create_connection((str(address), 2323), timeout=3)
            try:
                if wireless.get("tcp_cli_auth_required"):
                    self._authenticate(connection, sign_proof)
                connection.settimeout(0.1)
            except (OSError, RuntimeError, ValueError):
                connection.close()
                raise
            self._socket = connection
            self._session = secrets.token_urlsafe(32)
            self._last_request = time.monotonic()
            self._schedule_expiry(15)
            return self._session

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

    def _close(self):
        self._socket.close()
        self._socket = self._session = None

    def read(self, session):
        with self._lock:
            self._require_session(session)
            try:
                data = self._socket.recv(4096)
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
            if self._socket is not None:
                self._close()
