import unittest
from types import SimpleNamespace
from unittest.mock import patch

from host.direct.server import DirectHandler, DirectTerminal, _DEFAULT_WEB_ROOT


class DirectTerminalTests(unittest.TestCase):
    class FakeSocket:
        def __init__(self, challenge=b""):
            self.remaining = challenge
            self.sent = []
            self.closed = False

        def settimeout(self, _timeout):
            pass

        def recv(self, size):
            chunk, self.remaining = self.remaining[:size], self.remaining[size:]
            return chunk

        def sendall(self, data):
            self.sent.append(data)

        def close(self):
            self.closed = True

    CHALLENGE = b"PicoForge AUTH 000102030405060708090a0b0c0d0e0f 0001020304050607\r\n"

    def test_default_web_root_contains_only_browser_assets(self):
        self.assertEqual(_DEFAULT_WEB_ROOT.name, "web")
        self.assertTrue((_DEFAULT_WEB_ROOT / "index.html").is_file())
        self.assertFalse((_DEFAULT_WEB_ROOT / "server.py").exists())

    def test_open_rejects_non_lan_addresses(self):
        with self.assertRaisesRegex(ValueError, "private LAN"):
            DirectTerminal().open("8.8.8.8")

    def test_authenticated_open_waits_for_a_board_bound_proof_before_terminal_io(self):
        connection = self.FakeSocket(self.CHALLENGE)
        terminal = DirectTerminal()
        with patch("host.direct.server.socket.create_connection", return_value=connection), \
                patch.object(terminal, "_touch_locked"):
            opened = terminal.open("192.168.1.2")
            self.assertEqual(opened["challenge"], {
                "nonce": "000102030405060708090a0b0c0d0e0f", "uid": "0001020304050607",
            })
            with self.assertRaisesRegex(RuntimeError, "authentication is pending"):
                terminal.write(opened["session"], b"help\r")
            with self.assertRaisesRegex(ValueError, "Invalid TCP CLI proof"):
                terminal.authenticate(opened["session"], "not-a-proof")
            terminal.authenticate(opened["session"], "AB" * 16)
            self.assertEqual(connection.sent, [b"AUTH " + b"ab" * 16 + b"\r\n"])
            with self.assertRaisesRegex(RuntimeError, "not awaiting authentication"):
                terminal.authenticate(opened["session"], "cd" * 16)
            terminal.write(opened["session"], b"help\r")
            self.assertEqual(connection.sent[-1], b"help\r")
            terminal.close_all()
            self.assertTrue(connection.closed)

    def test_authenticated_open_rejects_malformed_board_challenges(self):
        for challenge in [b"invalid\r\n", b"PicoForge AUTH 00 0001020304050607\r\n",
                          b"PicoForge AUTH 000102030405060708090a0b0c0d0e0f 000102030405060g\r\n"]:
            with self.subTest(challenge=challenge):
                with self.assertRaisesRegex(ValueError, "Invalid TCP CLI authentication challenge"):
                    DirectTerminal._read_auth_challenge(self.FakeSocket(challenge))

    def test_terminal_api_rejects_nonlocal_or_cross_origin_requests(self):
        handler = object.__new__(DirectHandler)
        handler.server = SimpleNamespace(server_port=8000)
        handler.client_address = ("127.0.0.1", 50000)
        handler.headers = {"Host": "localhost:8000", "Origin": "http://localhost:8000"}
        self.assertTrue(handler._same_origin_loopback())
        handler.client_address = ("192.168.1.20", 50000)
        self.assertFalse(handler._same_origin_loopback())
        handler.client_address = ("127.0.0.1", 50000)
        handler.headers["Origin"] = "http://example.com"
        self.assertFalse(handler._same_origin_loopback())
        handler.headers["Origin"] = "http://localhost"
        self.assertFalse(handler._same_origin_loopback())

    @patch("host.direct.server.socket.create_connection")
    def test_open_uses_fixed_cli_port_and_closes_duplicate_connection(self, create_connection):
        first = self.FakeSocket(self.CHALLENGE)
        create_connection.return_value = first
        terminal = DirectTerminal()
        with patch.object(terminal, "_touch_locked"):
            opened = terminal.open("192.168.1.2")
            self.assertTrue(opened["session"])
            self.assertEqual(create_connection.call_args.args, (("192.168.1.2", 2323),))
            second = self.FakeSocket(self.CHALLENGE)
            create_connection.return_value = second
            with self.assertRaisesRegex(RuntimeError, "already connected"):
                terminal.open("192.168.1.3")
            self.assertTrue(second.closed)
            terminal.close_all()
            self.assertTrue(first.closed)


if __name__ == "__main__":
    unittest.main()
