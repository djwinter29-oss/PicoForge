import base64
import hashlib
import hmac
import socket
import threading
import time
import unittest
from unittest.mock import Mock, patch

from picoforge.dashboard.console import ConsoleBridge
from picoforge.dashboard.server import create_app, run_server


class Monitor:
    wireless = {"wifi_enabled": True, "wifi_state": "connected", "tcp_cli_started": True,
                "ipv4": "192.168.66.87"}

    def monitor(self, board):
        self.board = board
        return self

    def snapshot(self):
        return {"wireless": self.wireless}

    def read_wireless(self):
        raise AssertionError("console connection must use the monitor snapshot")

    def tcp_cli_auth_proof(self, nonce, uid):
        key = bytes(range(1, 33))
        return hmac.new(key, nonce + uid + b"PFTCPCLI1TCPCLI001", hashlib.sha256).digest()[:16]


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.client_socket, self.peer = socket.socketpair()
        self.peer.settimeout(1)
        self.addCleanup(self.client_socket.close)
        self.addCleanup(self.peer.close)
        self.connect = patch(
            "picoforge.dashboard.console.socket.create_connection",
            return_value=self.client_socket,
        ).start()
        patch("picoforge.dashboard.console._READ_WAIT_SECONDS", 0.2).start()
        self.addCleanup(patch.stopall)
        self.monitor = Monitor()
        self.app = create_app(self.monitor)
        self.client = self.app.test_client()

    def open(self):
        response = self.client.post("/api/console?board=board-a", json={})
        self.assertEqual(response.status_code, 200)
        return "/api/console?session=" + response.json["session"]

    def test_raw_bytes_round_trip_session_ownership_and_peer_close(self):
        endpoint = self.open()
        self.assertEqual(self.monitor.board, "board-a")
        self.connect.assert_called_once_with(("192.168.66.87", 2323), timeout=3)
        self.assertEqual(self.app.extensions["picoforge_console"]._board_id, "board-a")
        self.assertEqual(self.client.post("/api/console", json={}).status_code, 409)
        self.assertEqual(self.client.delete("/api/console?session=wrong").status_code, 409)
        command = b"help\r\x1b[A"
        response = self.client.put(endpoint, json={"data": base64.b64encode(command).decode()})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(self.peer.recv(1024), command)
        output = b"PicoForge> \x1b[D<script>\xff"
        self.peer.sendall(output)
        response = self.client.get(endpoint)
        self.assertEqual(base64.b64decode(response.json["data"]), output)
        self.assertFalse(response.json["closed"])
        self.assertEqual(self.client.get(endpoint).json, {"data": "", "closed": False})
        self.peer.close()
        self.assertTrue(self.client.get(endpoint).json["closed"])
        self.assertEqual(self.client.get(endpoint).status_code, 409)

    def test_waiting_read_does_not_block_input_and_returns_board_output(self):
        bridge = ConsoleBridge()
        token = bridge.open(self.monitor.wireless)
        result = []
        with patch("picoforge.dashboard.console._READ_WAIT_SECONDS", 1):
            reader = threading.Thread(target=lambda: result.append(bridge.read(token)))
            reader.start()
            time.sleep(0.05)
            started = time.monotonic()
            bridge.write(token, b"x")
            self.assertLess(time.monotonic() - started, 0.5)
            self.assertEqual(self.peer.recv(1), b"x")
            self.peer.sendall(b"ok")
            reader.join(2)
        self.assertEqual(result, [(b"ok", False)])
        bridge.close(token)

    def test_close_all_does_not_wait_for_socket_connect(self):
        started = threading.Event()
        release = threading.Event()
        connection = Mock()
        bridge = ConsoleBridge()
        errors = []

        def delayed_connect(*_args, **_kwargs):
            started.set()
            release.wait(2)
            return connection

        def open_console():
            try:
                bridge.open(self.monitor.wireless, board_id="board-a")
            except RuntimeError as error:
                errors.append(error)

        with patch("picoforge.dashboard.console.socket.create_connection", side_effect=delayed_connect):
            worker = threading.Thread(target=open_console)
            worker.start()
            self.assertTrue(started.wait(1))
            before = time.monotonic()
            bridge.close_all()
            self.assertLess(time.monotonic() - before, 0.2)
            release.set()
            worker.join(2)

        self.assertFalse(worker.is_alive())
        self.assertEqual(len(errors), 1)
        connection.close.assert_called_once_with()
        self.assertIsNone(bridge._socket)

    def test_input_limits_origins_and_explicit_disconnect(self):
        endpoint = self.open()
        for payload in ({"data": "?"}, {"data": ""}, {"data": "a" * 1369},
                        {"data": 1}, {"data": "YQ==", "extra": True}):
            self.assertEqual(self.client.put(endpoint, json=payload).status_code, 400)
        self.assertEqual(self.client.put(endpoint, json={"data": "YQ=="},
                                         headers={"Origin": "https://untrusted.example"}).status_code, 403)
        self.assertEqual(self.client.delete(endpoint).status_code, 200)
        self.assertEqual(self.peer.recv(1), b"")

    def test_unavailable_listener_does_not_connect_and_page_is_packaged(self):
        self.monitor.wireless = {**self.monitor.wireless, "tcp_cli_started": False}
        self.assertEqual(self.client.post("/api/console", json={}).status_code, 409)
        self.connect.assert_not_called()
        with self.client.get("/console?board=board-a") as page:
            self.assertEqual(page.status_code, 200)
            self.assertIn(b"/js/console.mjs", page.data)

    def test_abandoned_session_expires_and_connection_errors_are_explicit(self):
        bridge = ConsoleBridge()
        token = bridge.open(self.monitor.wireless)
        bridge._last_request = time.monotonic() - 16
        bridge._expire()
        with self.assertRaises(RuntimeError):
            bridge.read(token)
        self.connect.side_effect = OSError("connection refused")
        self.assertEqual(self.client.post("/api/console", json={}).status_code, 503)

    def test_close_all_disconnects_an_active_console_session(self):
        endpoint = self.open()
        bridge = self.app.extensions["picoforge_console"]
        bridge.close_all()
        self.assertEqual(self.peer.recv(1), b"")
        with self.assertRaises(RuntimeError):
            bridge.read(endpoint.split("session=", 1)[1])

    def test_server_shutdown_closes_console_and_dashboard(self):
        dashboard = Mock()
        connection = Mock()

        def stop_server(app, **_kwargs):
            app.extensions["picoforge_console"]._socket = connection
            app.extensions["picoforge_console"]._session = "active"
            raise RuntimeError("server stopped")

        with patch("picoforge.dashboard.server.DashboardBoards", return_value=dashboard), \
                patch("picoforge.dashboard.server.Flask.run", autospec=True,
                   side_effect=stop_server):
            with self.assertRaisesRegex(RuntimeError, "server stopped"):
                run_server()

        connection.close.assert_called_once()
        dashboard.close.assert_called_once()

    def test_invalid_board_addresses_are_not_used_as_proxy_targets(self):
        for ip in ("127.0.0.1", "224.0.0.1", "0.0.0.0", "255.255.255.255", "bad", "::1"):
            self.monitor.wireless = {**self.monitor.wireless, "ipv4": ip}
            self.assertEqual(self.client.post("/api/console", json={}).status_code, 503, "board reported bad IP")
        self.connect.assert_not_called()

    def test_tcp_authentication_uses_the_connected_boards_usb_proof(self):
        self.monitor.wireless = {**self.monitor.wireless, "ble_key_configured": True}
        nonce = bytes(range(16))
        uid = bytes(range(8))
        self.peer.sendall(f"PicoForge AUTH {nonce.hex()} {uid.hex()}\r\n".encode("ascii"))
        response = self.client.post("/api/console?board=board-a", json={})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(self.monitor.board, "board-a")
        key = bytes(range(1, 33))
        proof = hmac.new(key, nonce + uid + b"PFTCPCLI1" + b"TCPCLI001", hashlib.sha256).digest()[:16]
        self.assertEqual(self.peer.recv(64), b"AUTH " + proof.hex().encode() + b"\r\n")
        endpoint = "/api/console?session=" + response.json["session"]
        self.assertEqual(self.client.delete(endpoint).status_code, 200)

    def test_tcp_authentication_rejects_whitespace_padded_challenge_hex(self):
        self.monitor.wireless = {**self.monitor.wireless, "ble_key_configured": True}
        self.peer.sendall(f"PicoForge AUTH {'00' * 15}\t\t {'00' * 8}\r\n".encode("ascii"))
        self.assertEqual(self.client.post("/api/console", json={}).status_code, 503)

    def test_tcp_authentication_does_not_accept_control_keys_from_the_browser(self):
        self.monitor.wireless = {**self.monitor.wireless, "ble_key_configured": True}
        self.assertEqual(self.client.post("/api/console", json={"ble_key": "00" * 32}).status_code, 400)
        self.assertEqual(self.client.post("/api/console", json={"ble_key": "invalid"}).status_code, 400)
        self.connect.assert_not_called()
