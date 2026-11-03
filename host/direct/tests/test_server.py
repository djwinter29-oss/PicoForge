import hashlib
import hmac
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from tools.host.direct_server import DirectHandler, DirectTerminal


class DirectTerminalTests(unittest.TestCase):
    def test_authentication_key_must_be_nonzero_256_bit_hex(self):
        self.assertTrue(DirectTerminal._valid_key("ab" * 32))
        self.assertFalse(DirectTerminal._valid_key("00" * 32))
        self.assertFalse(DirectTerminal._valid_key("ab" * 31))
        self.assertFalse(DirectTerminal._valid_key("zz" * 32))

    def test_open_rejects_non_lan_addresses_and_missing_authentication_key(self):
        terminal = DirectTerminal()
        with self.assertRaisesRegex(ValueError, "private LAN"):
            terminal.open("8.8.8.8", False)
        with self.assertRaisesRegex(ValueError, "control key"):
            terminal.open("192.168.1.2", True, "")

    def test_authentication_proof_matches_board_challenge(self):
        key = "ab" * 32
        nonce = bytes(range(16))
        uid = bytes(range(8))
        challenge = f"PicoForge AUTH {nonce.hex()} {uid.hex()}\r\n".encode("ascii")

        class FakeSocket:
            def __init__(self):
                self.remaining = challenge
                self.sent = b""

            def settimeout(self, _timeout):
                pass

            def recv(self, size):
                chunk, self.remaining = self.remaining[:size], self.remaining[size:]
                return chunk

            def sendall(self, data):
                self.sent = data

        connection = FakeSocket()
        DirectTerminal._authenticate(connection, key)
        message = nonce + uid + b"PFTCPCLI1" + b"TCPCLI001"
        proof = hmac.new(bytes.fromhex(key), message, hashlib.sha256).digest()[:16]
        self.assertEqual(connection.sent, b"AUTH " + proof.hex().encode("ascii") + b"\r\n")

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

    @patch("tools.host.direct_server.socket.create_connection")
    def test_open_uses_fixed_cli_port_and_closes_duplicate_connection(self, create_connection):
        first = create_connection.return_value
        terminal = DirectTerminal()
        with patch.object(terminal, "_touch_locked"):
            session = terminal.open("192.168.1.2", False)
            self.assertTrue(session)
            self.assertEqual(create_connection.call_args.args, (("192.168.1.2", 2323),))
            second = type(first)()
            create_connection.return_value = second
            with self.assertRaisesRegex(RuntimeError, "already connected"):
                terminal.open("192.168.1.3", False)
            second.close.assert_called_once()
            terminal.close_all()


if __name__ == "__main__":
    unittest.main()
