#!/usr/bin/env python3
"""Serve the Direct dashboard and relay its local terminal to the board TCP CLI."""

from __future__ import annotations

import argparse
import base64
from functools import partial
import ipaddress
import json
import secrets
import socket
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


_SOURCE_ROOT = Path(__file__).resolve().parent
_DEFAULT_WEB_ROOT = _SOURCE_ROOT / "web" if (_SOURCE_ROOT / "web" / "index.html").is_file() \
    else Path(__file__).resolve().parents[2] / "host" / "direct" / "web"


class DirectTerminal:
    """Keep one authenticated TCP CLI session for the Direct dashboard."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._socket: socket.socket | None = None
        self._session: str | None = None
        self._authenticated = False
        self._last_request = 0.0
        self._timer: threading.Timer | None = None

    @staticmethod
    def _read_auth_challenge(connection: socket.socket) -> dict[str, str]:
        deadline = time.monotonic() + 3
        challenge = bytearray()
        while len(challenge) < 128 and not challenge.endswith(b"\r\n"):
            timeout = deadline - time.monotonic()
            if timeout <= 0:
                raise TimeoutError("TCP CLI authentication challenge timed out")
            connection.settimeout(timeout)
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
                or any(char not in "0123456789abcdefABCDEF" for char in fields[2])
                or len(fields[3]) != 16
                or any(char not in "0123456789abcdefABCDEF" for char in fields[3])):
            raise ValueError("Invalid TCP CLI authentication challenge")
        return {"nonce": fields[2].lower(), "uid": fields[3].lower()}

    def open(self, address: object) -> dict[str, object]:
        """Connect and read the challenge; firmware always requires TCP CLI authentication."""
        try:
            ip = ipaddress.IPv4Address(address)
        except (ipaddress.AddressValueError, TypeError) as error:
            raise ValueError("Invalid board IPv4 address") from error
        if not ip.is_private or ip.is_loopback or ip.is_multicast or ip.is_unspecified:
            raise ValueError("The board address must be a private LAN IPv4 address")

        connection = socket.create_connection((str(ip), 2323), timeout=3)
        try:
            challenge = self._read_auth_challenge(connection)
            connection.settimeout(0.1)
            with self._lock:
                if self._socket is not None:
                    raise RuntimeError("A Direct terminal is already connected")
                self._socket = connection
                self._session = secrets.token_urlsafe(32)
                self._authenticated = False
                self._touch_locked()
                return {"session": self._session, "challenge": challenge}
        except (OSError, ValueError, RuntimeError):
            connection.close()
            raise

    def authenticate(self, session: object, proof: object) -> None:
        if (not isinstance(proof, str) or len(proof) != 32
                or any(char not in "0123456789abcdefABCDEF" for char in proof)):
            raise ValueError("Invalid TCP CLI proof")
        with self._lock:
            connection = self._require_session_locked(session, allow_pending_auth=True)
            if self._authenticated:
                raise RuntimeError("Terminal session is not awaiting authentication")
            try:
                connection.sendall(b"AUTH " + proof.lower().encode("ascii") + b"\r\n")
            except OSError:
                self._close_locked()
                raise
            self._authenticated = True
            self._touch_locked()

    def _touch_locked(self) -> None:
        self._last_request = time.monotonic()
        if self._timer is not None:
            return
        self._timer = threading.Timer(15, self._expire, args=(self._session,))
        self._timer.daemon = True
        self._timer.start()

    def _expire(self, session: str | None) -> None:
        with self._lock:
            if self._socket is None or session != self._session:
                return
            self._timer = None
            remaining = 15 - (time.monotonic() - self._last_request)
            if remaining > 0:
                self._timer = threading.Timer(remaining, self._expire, args=(self._session,))
                self._timer.daemon = True
                self._timer.start()
            else:
                self._close_locked()

    def _require_session_locked(self, session: object, *, allow_pending_auth: bool = False) -> socket.socket:
        if (self._socket is None or not isinstance(session, str)
                or not secrets.compare_digest(self._session or "", session)):
            raise RuntimeError("Terminal disconnected; connect again")
        if not self._authenticated and not allow_pending_auth:
            raise RuntimeError("Terminal authentication is pending")
        self._touch_locked()
        return self._socket

    def _close_locked(self) -> None:
        timer, connection = self._timer, self._socket
        self._timer = None
        self._socket = None
        self._session = None
        self._authenticated = False
        if timer is not None:
            timer.cancel()
        if connection is not None:
            connection.close()

    def read(self, session: object) -> tuple[bytes, bool]:
        with self._lock:
            connection = self._require_session_locked(session)
            try:
                data = connection.recv(4096)
            except socket.timeout:
                return b"", False
            except OSError:
                self._close_locked()
                raise
            if not data:
                self._close_locked()
            return data, not data

    def write(self, session: object, data: bytes) -> None:
        with self._lock:
            connection = self._require_session_locked(session)
            try:
                connection.sendall(data)
            except OSError:
                self._close_locked()
                raise

    def close(self, session: object) -> None:
        with self._lock:
            self._require_session_locked(session, allow_pending_auth=True)
            self._close_locked()

    def close_all(self) -> None:
        with self._lock:
            self._close_locked()


_TERMINAL = DirectTerminal()


class DirectHandler(SimpleHTTPRequestHandler):
    """Serve static Direct files and same-origin local terminal requests."""

    def end_headers(self) -> None:
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
                         "style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; "
                         "frame-ancestors 'none'")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        super().end_headers()

    def _same_origin_loopback(self) -> bool:
        host = self.headers.get("Host", "")
        try:
            origin = urlsplit(self.headers.get("Origin", ""))
            return (ipaddress.ip_address(self.client_address[0]).is_loopback
                    and origin.scheme == "http" and origin.netloc == host
                    and origin.hostname in {"localhost", "127.0.0.1"}
                    and origin.port == self.server.server_port)
        except (ValueError, TypeError):
            return False

    def _send_json(self, status: int, payload: dict[str, object]) -> None:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict[str, object]:
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError as error:
            raise ValueError("Invalid request length") from error
        if not 0 < length <= 8192:
            raise ValueError("Invalid terminal request size")
        try:
            payload = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise ValueError("Invalid terminal request JSON") from error
        if not isinstance(payload, dict):
            raise ValueError("Invalid terminal request")
        return payload

    def do_POST(self) -> None:
        if not self._same_origin_loopback():
            self._send_json(403, {"error": "Terminal access requires this local Direct dashboard"})
            return
        try:
            payload = self._read_json()
            if self.path == "/api/direct-terminal/open":
                opened = _TERMINAL.open(payload.get("ipv4"))
                self._send_json(200, opened)
            elif self.path == "/api/direct-terminal/auth":
                _TERMINAL.authenticate(payload.get("session"), payload.get("proof"))
                self._send_json(200, {"authenticated": True})
            elif self.path == "/api/direct-terminal/read":
                data, closed = _TERMINAL.read(payload.get("session"))
                self._send_json(200, {"data": base64.b64encode(data).decode("ascii"), "closed": closed})
            elif self.path == "/api/direct-terminal/write":
                session = payload.get("session")
                encoded = payload.get("data")
                if not isinstance(encoded, str):
                    raise ValueError("Invalid terminal data")
                try:
                    data = base64.b64decode(encoded, validate=True)
                except ValueError as error:
                    raise ValueError("Invalid terminal data") from error
                if not data or len(data) > 4096:
                    raise ValueError("Terminal input must contain 1-4096 bytes")
                _TERMINAL.write(session, data)
                self._send_json(200, {"written": len(data)})
            elif self.path == "/api/direct-terminal/close":
                _TERMINAL.close(payload.get("session"))
                self._send_json(200, {"closed": True})
            else:
                self._send_json(404, {"error": "Unknown terminal operation"})
        except ValueError as error:
            self._send_json(400, {"error": str(error)})
        except RuntimeError as error:
            self._send_json(409, {"error": str(error)})
        except OSError as error:
            self._send_json(502, {"error": f"Board TCP CLI request failed: {error}"})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="Local bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8000, help="Local HTTP port (default: 8000)")
    parser.add_argument("--directory", type=Path, default=_DEFAULT_WEB_ROOT,
                        help="Dashboard directory (defaults to the adjacent files or repository dashboard)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not args.directory.is_dir() or not (args.directory / "index.html").is_file():
        parser.error(f"dashboard directory must contain index.html: {args.directory}")
    server = ThreadingHTTPServer((args.bind, args.port),
                                 partial(DirectHandler, directory=str(args.directory)))
    server.daemon_threads = True
    host = "127.0.0.1" if args.bind == "0.0.0.0" else args.bind
    print(f"Direct dashboard: http://{host}:{args.port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            _TERMINAL.close_all()
        finally:
            server.server_close()


if __name__ == "__main__":
    main()
