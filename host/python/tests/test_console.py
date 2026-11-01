import base64
import socket
import time
import unittest
from unittest.mock import patch

from picoforge.console import ConsoleBridge
from picoforge.web import create_app


class Monitor:
    wireless = {"wifi_enabled": True, "wifi_state": "connected", "tcp_cli_started": True,
                "ipv4": "192.168.66.87"}

    def read_wireless(self, board):
        self.board = board
        return {"wireless": self.wireless}


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.client_socket, self.peer = socket.socketpair()
        self.peer.settimeout(1)
        self.addCleanup(self.client_socket.close)
        self.addCleanup(self.peer.close)
        self.connect = patch("picoforge.console.socket.create_connection", return_value=self.client_socket).start()
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

    def test_invalid_board_addresses_are_not_used_as_proxy_targets(self):
        for ip in ("127.0.0.1", "224.0.0.1", "0.0.0.0", "255.255.255.255", "bad", "::1"):
            self.monitor.wireless = {**self.monitor.wireless, "ipv4": ip}
            self.assertEqual(self.client.post("/api/console", json={}).status_code, 400)
        self.connect.assert_not_called()
