# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

import base64
import hashlib
import hmac
import socket
import threading
import time
import unittest
from unittest.mock import Mock, patch

from helpers import make_monitor
from picoforge.dashboard.console import ConsoleBridge
from picoforge.dashboard.server import create_app, run_server
from picoforge.reports.common import BoardConflict


class Monitor:
    wireless = {"wifi_enabled": True, "wifi_state": "connected", "tcp_cli_started": True,
                "ipv4": "192.168.66.87"}

    def monitor(self, board):
        self.board = board
        return self

    def snapshot(self):
        return {"connected": True, "wireless": self.wireless}

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
        self.addCleanup(self.app.extensions["picoforge_console"].close_all)
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

    def test_shutdown_during_proof_closes_opening_socket_and_never_sends_authentication(self):
        for fail in (False, True):
            with self.subTest(fail=fail):
                connection, peer = socket.socketpair()
                self.addCleanup(connection.close)
                self.addCleanup(peer.close)
                peer.settimeout(1)
                peer.sendall(b"PicoForge AUTH " + b"00" * 16 + b" " + b"00" * 8 + b"\r\n")
                bridge = ConsoleBridge()
                entered, release = threading.Event(), threading.Event()
                errors = []

                def proof(*_args):
                    entered.set()
                    if not release.wait(2):
                        raise AssertionError("proof release timed out")
                    if fail:
                        raise ValueError("proof programmer bug")
                    return bytes(16)

                def opening():
                    try:
                        bridge.open({**self.monitor.wireless, "ble_key_configured": True}, proof)
                    except Exception as error:
                        errors.append(error)

                with patch("picoforge.dashboard.console.socket.create_connection", return_value=connection):
                    worker = threading.Thread(target=opening)
                    worker.start()
                    try:
                        self.assertTrue(entered.wait(1))
                        bridge.close_all()
                        self.assertEqual(connection.fileno(), -1)
                        release.set()
                        worker.join(2)
                        self.assertFalse(worker.is_alive())
                        self.assertEqual(peer.recv(128), b"")
                        self.assertIsNone(bridge._socket)
                        self.assertIsNone(bridge._opening)
                        self.assertEqual(len(errors), 1)
                        self.assertIsInstance(errors[0], ValueError if fail else BoardConflict)
                    finally:
                        release.set()
                        worker.join(2)
                        bridge.close_all()

    def test_shutdown_during_challenge_prevents_usb_proof_and_session_publication(self):
        entered, release = threading.Event(), threading.Event()
        connection = Mock()
        bridge = ConsoleBridge()
        sign = Mock(return_value=bytes(16))
        errors = []

        def receive(_size):
            entered.set()
            if not release.wait(2):
                raise AssertionError("challenge release timed out")
            raise OSError("closed while reading challenge")

        def opening():
            try:
                bridge.open({**self.monitor.wireless, "ble_key_configured": True}, sign)
            except Exception as error:
                errors.append(error)

        connection.recv.side_effect = receive
        with patch("picoforge.dashboard.console.socket.create_connection", return_value=connection):
            worker = threading.Thread(target=opening)
            worker.start()
            try:
                self.assertTrue(entered.wait(1))
                bridge.close_all()
                connection.close.assert_called_once()
                release.set()
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(len(errors), 1)
                self.assertIsInstance(errors[0], BoardConflict)
                sign.assert_not_called()
                connection.sendall.assert_not_called()
                connection.close.assert_called_once()
            finally:
                release.set()
                worker.join(2)
                bridge.close_all()

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

    def test_disconnected_failed_or_expired_wireless_cannot_authorize_console_connections(self):
        wireless = self.monitor.wireless
        for snapshot in (
            {"connected": False, "wireless": wireless},
            {"connected": True, "wireless_error": "Feature 12 failed", "wireless": wireless},
            {"connected": True, "wireless": {**wireless, "stale": True}},
            {"connected": True, "wireless_cleanup_required": True, "wireless": wireless},
        ):
            with self.subTest(snapshot=snapshot), patch.object(self.monitor, "snapshot", return_value=snapshot):
                response = self.client.post("/api/console?board=board-a", json={})
                self.assertEqual(response.status_code, 409)
                self.assertTrue(response.json["error"])
        self.connect.assert_not_called()

    def test_receipt_age_gates_the_real_monitor_console_route_and_fresh_refresh_restores_it(self):
        monitor = make_monitor(snapshot={"connected": True, "wireless": dict(self.monitor.wireless)}, fresh=False)
        monitor._wireless_received_at = 100.0
        boards = Mock()
        boards.monitor.return_value = monitor
        app = create_app(boards)
        self.addCleanup(app.extensions["picoforge_console"].close_all)
        http = app.test_client()
        for now in (106.0, 1000.0):
            with self.subTest(now=now), patch("picoforge.dashboard.monitor.time.monotonic", return_value=now):
                response = http.get("/api/status?board=a")
                self.assertTrue(response.json["wireless"]["stale"])
                self.assertTrue(response.json["wireless_error"])
                self.assertEqual(http.post("/api/console?board=a", json={}).status_code, 409)
        self.connect.assert_not_called()
        with patch("picoforge.dashboard.monitor.time.monotonic", return_value=1000.0):
            monitor._publish_wireless(dict(self.monitor.wireless))
            self.assertFalse(http.get("/api/status?board=a").json["wireless"].get("stale", False))
            self.assertEqual(http.post("/api/console?board=a", json={}).status_code, 200)
        self.connect.assert_called_once()

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

    def test_unterminated_challenge_is_a_protocol_error_not_an_internal_bug(self):
        self.monitor.wireless = {**self.monitor.wireless, "ble_key_configured": True}
        self.peer.sendall(b"x" * 128)
        response = self.client.post("/api/console", json={})
        self.assertEqual(response.status_code, 503)
        self.assertIn("Invalid TCP CLI authentication challenge", response.json["error"])
        self.assertEqual(self.peer.recv(1), b"")

    def test_challenge_uses_one_deadline_instead_of_a_timeout_per_byte(self):
        connection = Mock()
        clock = [100.0]
        timeouts = []
        connection.settimeout.side_effect = timeouts.append

        def receive(_count):
            clock[0] += 1.0
            return b"x"

        connection.recv.side_effect = receive
        sign = Mock()
        with patch("picoforge.dashboard.console.time.monotonic", side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(TimeoutError, "challenge timed out"):
                ConsoleBridge._authenticate(connection, sign)
        self.assertEqual(timeouts, [3.0, 2.0, 1.0])
        self.assertEqual(connection.recv.call_count, 3)
        sign.assert_not_called()

    def test_authentication_proof_and_write_share_the_overall_budget(self):
        challenge = b"PicoForge AUTH " + b"00" * 16 + b" " + b"00" * 8 + b"\r\n"
        for proof_elapsed, succeeds in ((4.0, True), (8.0, False)):
            with self.subTest(proof_elapsed=proof_elapsed):
                clock = [100.0]
                connection = Mock()
                connection.recv.side_effect = [bytes((value,)) for value in challenge]

                def proof(*_):
                    clock[0] += proof_elapsed
                    return bytes(16)

                with patch("picoforge.dashboard.console.time.monotonic", side_effect=lambda: clock[0]):
                    if succeeds:
                        ConsoleBridge._authenticate(connection, proof)
                        self.assertEqual(connection.settimeout.call_args.args, (4.0,))
                        connection.sendall.assert_called_once()
                    else:
                        with self.assertRaisesRegex(TimeoutError, "authentication timed out"):
                            ConsoleBridge._authenticate(connection, proof)
                        connection.sendall.assert_not_called()

    def test_poll_failure_closes_session_and_returns_unavailable(self):
        endpoint = self.open()
        with patch("picoforge.dashboard.console.select.select", side_effect=OSError("poll failed")):
            response = self.client.get(endpoint)
        self.assertEqual(response.status_code, 503)
        self.assertIn("poll failed", response.json["error"])
        self.assertEqual(self.client.get(endpoint).status_code, 409)
        self.assertEqual(self.peer.recv(1), b"")

    def test_poll_invalid_descriptor_is_unavailable_but_a_close_race_is_conflict(self):
        endpoint = self.open()
        with patch("picoforge.dashboard.console.select.select", side_effect=ValueError("bad descriptor")):
            self.assertEqual(self.client.get(endpoint).status_code, 503)
        bridge = ConsoleBridge()
        replacement, peer = socket.socketpair()
        self.addCleanup(peer.close)
        with patch("picoforge.dashboard.console.socket.create_connection", return_value=replacement):
            session = bridge.open(self.monitor.wireless)

        def closed_during_select(*_):
            bridge.close_all()
            raise ValueError("closed descriptor")

        with patch("picoforge.dashboard.console.select.select", side_effect=closed_during_select):
            with self.assertRaisesRegex(RuntimeError, "disconnected"):
                bridge.read(session)

    def test_expiry_timer_is_cancelled_on_disconnect_and_cannot_expire_a_replacement(self):
        bridge = ConsoleBridge()
        with patch("picoforge.dashboard.console.threading.Timer") as timer_factory:
            old_timer, new_timer = Mock(), Mock()
            timer_factory.side_effect = [old_timer, new_timer]
            first = bridge.open(self.monitor.wireless)
            bridge.close(first)
            old_timer.cancel.assert_called_once()
            replacement, peer = socket.socketpair()
            self.addCleanup(peer.close)
            with patch("picoforge.dashboard.console.socket.create_connection", return_value=replacement):
                second = bridge.open(self.monitor.wireless)
            bridge._expire(first)
            self.assertEqual(bridge._session, second)
            bridge.close_all()
            new_timer.cancel.assert_called_once()
            self.assertIsNone(bridge._expiry_timer)
            with self.assertRaisesRegex(RuntimeError, "shutting down"):
                bridge.open(self.monitor.wireless)

    def test_tcp_authentication_does_not_accept_control_keys_from_the_browser(self):
        self.monitor.wireless = {**self.monitor.wireless, "ble_key_configured": True}
        self.assertEqual(self.client.post("/api/console", json={"ble_key": "00" * 32}).status_code, 400)
        self.assertEqual(self.client.post("/api/console", json={"ble_key": "invalid"}).status_code, 400)
        self.connect.assert_not_called()
