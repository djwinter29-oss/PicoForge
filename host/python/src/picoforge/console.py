"""Bounded, single-session bridge from the local dashboard to the board's raw TCP CLI."""

from __future__ import annotations

import ipaddress
import hashlib
import hmac
import secrets
import socket
import threading
import time


class ConsoleBridge:
    """Relay raw bytes; close abandoned tabs after 15 seconds without a request."""

    def __init__(self):
        self._lock = threading.Lock()
        self._socket = None
        self._session = None
        self._last_request = 0

    @staticmethod
    def _valid_key(ble_key):
        if not isinstance(ble_key, str) or len(ble_key) != 64 or any(
                character not in "0123456789abcdefABCDEF" for character in ble_key):
            return False
        key = bytearray.fromhex(ble_key)
        try:
            return any(key)
        finally:
            key[:] = bytes(len(key))

    @staticmethod
    def _authenticate(connection, ble_key):
        if not ConsoleBridge._valid_key(ble_key):
            raise ValueError("A 64-character nonzero BLE control key is required for TCP CLI authentication")
        key = bytearray.fromhex(ble_key)
        message = bytearray()
        proof = bytearray()
        try:
            connection.settimeout(3)
            challenge = bytearray()
            while len(challenge) < 128 and not challenge.endswith(b"\r\n"):
                chunk = connection.recv(1)
                if not chunk:
                    raise OSError("Board closed the TCP CLI during authentication")
                challenge.extend(chunk)
            if not challenge.endswith(b"\r\n"):
                raise ValueError("Invalid TCP CLI authentication challenge")
            fields = challenge[:-2].decode("ascii").split(" ")
            if (len(fields) != 4 or fields[:2] != ["PicoForge", "AUTH"] or len(fields[2]) != 32
                    or any(character not in "0123456789abcdefABCDEF" for character in fields[2])
                    or len(fields[3]) != 16
                    or any(character not in "0123456789abcdefABCDEF" for character in fields[3])):
                raise ValueError("Invalid TCP CLI authentication challenge")
            nonce = bytes.fromhex(fields[2])
            uid = bytes.fromhex(fields[3])
            message.extend(nonce + uid + b"PFTCPCLI1" + b"TCPCLI001")
            proof.extend(hmac.new(key, message, hashlib.sha256).digest()[:16])
            connection.sendall(b"AUTH " + proof.hex().encode("ascii") + b"\r\n")
        finally:
            key[:] = bytes(len(key))
            message[:] = bytes(len(message))
            proof[:] = bytes(len(proof))

    def open(self, wireless, ble_key=None):
        if not (wireless.get("wifi_enabled") and wireless.get("wifi_state") == "connected"
                and wireless.get("tcp_cli_started")):
            raise RuntimeError("The board's Wi-Fi TCP CLI is not available")
        address = ipaddress.IPv4Address(wireless["ipv4"])
        if address.is_loopback or address.is_multicast or address.is_unspecified or int(address) == 0xFFFFFFFF:
            raise ValueError("Invalid board IPv4 address")
        if wireless.get("tcp_cli_auth_required") and not self._valid_key(ble_key):
            raise ValueError("A 64-character nonzero BLE control key is required for TCP CLI authentication")
        with self._lock:
            if self._socket is not None:
                raise RuntimeError("A web console is already connected; disconnect it first")
            connection = socket.create_connection((str(address), 2323), timeout=3)
            try:
                if wireless.get("tcp_cli_auth_required"):
                    self._authenticate(connection, ble_key)
                connection.settimeout(0.1)
            except (OSError, ValueError):
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
