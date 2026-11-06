"""Contract and lifecycle checks for USB Features 12 and 20."""

import json
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from helpers import make_monitor
from picoforge.dashboard.server import create_app
from picoforge.dashboard.monitor import CommandOutcomeUncertain
from picoforge.hid.device import PicoForgeHid, WirelessTestCancelled
from picoforge.reports import (
    CONTROL_REPORT_ID, WIRELESS_REPORT_ID, build_tcp_cli_auth_request,
    build_wireless_command_report, build_settings_command_report,
    ProtocolError, parse_control_result_report, parse_wireless_status_report,
    validate_wireless_configuration,
)

BOARD = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 3, "tcp_cli_enabled": True}


def settings(**overrides):
    return {"wifi_enabled": True, "ble_enabled": True, "ssid": "Pico網",
            "confirm_reboot": True, "board": BOARD, **overrides}


def status(*, wifi=2, flags=3, ssid="Pico網", scan=0, generation=0, count=0, test=0, staged=0, detail=0):
    raw = ssid.encode() if isinstance(ssid, str) else ssid
    report = bytearray(49)
    report[:6] = bytes((12, 1, wifi, detail, 0, flags))
    if wifi in (2, 5):
        report[6:10] = bytes((192, 168, 1, 2))
    report[10] = len(raw)
    report[11:11 + len(raw)] = raw
    report[43:49] = bytes((scan, generation & 255, generation >> 8, count, test, staged))
    return bytes(report)


def result(opcode, transaction=1, *, state=2, error=0, data=b""):
    return bytes((20, 1, transaction, opcode, state, error)) + data.ljust(63, b"\0")


class WirelessTests(unittest.TestCase):
    def client(self, reports=()):
        client = PicoForgeHid.__new__(PicoForgeHid)
        client.closed = False
        client._close_error = None
        client.identity = {"supported": True, "capabilities": 0xF5, "serial_number": "0123456789ABCDEF"}
        client.configuration = {"radios": 3, "tcp_cli_enabled": True}
        client._transaction_id = 0
        client._command_uncertain = False
        client._uncertain_command = None
        client._uncertain_wireless_operation = None
        client._control_lock = threading.RLock()
        client.last_command_result = None
        client._wireless_active = False
        client._tested_generation = None
        client._tested_at = None
        client._device = Mock()
        client._device.send_feature_report.side_effect = lambda value: len(value)
        client._device.get_feature_report.side_effect = list(reports)
        return client

    def test_exact_status_layout_and_opaque_ssid(self):
        raw = b"Wi\0\xff\n" + "網".encode()
        parsed = parse_wireless_status_report(status(ssid=raw))
        self.assertEqual(parsed["ssid"], "Wi\\x00\\xFF\\x0A網")
        self.assertEqual(bytes.fromhex(parsed["ssid_hex"]), raw)
        self.assertEqual(parsed["wifi_state"], "connected")
        self.assertEqual(parsed["ipv4"], "192.168.1.2")
        self.assertNotIn("password_configured", parsed)
        self.assertNotIn("tcp_cli_enabled", parsed)
        self.assertNotIn("error", parsed)
        self.assertNotIn("tcp_cli_auth_required", parsed)
        self.assertNotIn("age_ms", parsed)
        self.assertNotIn("rssi", parsed)
        command = build_wireless_command_report(9, 6, value=bytes.fromhex(parsed["ssid_hex"]))
        self.assertEqual(command[:6], bytes((20, 1, 9, 6, 0, len(raw))))
        self.assertEqual(command[6:6 + len(raw)], raw)
        self.assertEqual(len(command), 69)
        self.assertEqual(parse_wireless_status_report(status(ssid=b"a" * 32))["ssid_hex"], "61" * 32)
        # Reserved flags/detail are ignored, as specified for Feature 12.
        self.assertEqual(parse_wireless_status_report(status(flags=11, detail=255))["detail"], 0)
        for padded in (b"\0" + status(), status() + b"\0"):
            with self.assertRaises(ValueError):
                parse_wireless_status_report(padded)

    def test_shared_firmware_status_vectors(self):
        vectors = json.loads((Path(__file__).resolve().parents[3] / "tests/fixtures/wireless-status.json").read_text())
        serving = parse_wireless_status_report(b"\x0c" + bytes.fromhex(vectors["tcp_cli_auth_required"]))
        disabled = parse_wireless_status_report(b"\x0c" + bytes.fromhex(vectors["tcp_cli_disabled"]))
        tested = parse_wireless_status_report(b"\x0c" + bytes.fromhex(vectors["tested_candidate"]))
        self.assertEqual(serving["ssid_hex"], b"PicoForge".hex())
        self.assertTrue(serving["tcp_cli_started"])
        self.assertTrue(serving["ble_key_configured"], "the console must answer the board's AUTH challenge")
        self.assertFalse(disabled["tcp_cli_started"])
        self.assertEqual((tested["wifi_state"], tested["test_generation"], tested["pair_tested"]),
                         ("test_joined", 1, True))
        lost = parse_wireless_status_report(b"\x0c" + bytes.fromhex(vectors["test_failed_saved_link_serving"]))
        self.assertEqual((lost["wifi_state"], lost["ipv4"], lost["pair_tested"], lost["tcp_cli_started"]),
                         ("test_failed", "42.42.42.42", True, True))
        init = parse_wireless_status_report(b"\x0c" + bytes.fromhex(vectors["radio_init_failed_while_connected"]))
        self.assertEqual((init["wifi_state"], init["detail"]), ("connected", 1))

    def test_status_cross_field_validation(self):
        for kwargs in (
            {"wifi": 7}, {"wifi": 4}, {"scan": 1}, {"count": 1},
            {"scan": 1, "generation": 1, "count": 1}, {"scan": 2, "generation": 1, "count": 19},
            {"flags": 0x13, "staged": 8}, {"staged": 1}, {"flags": 0x13, "staged": 1},
            {"flags": 0x83}, {"flags": 5}, {"staged": 16},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                parse_wireless_status_report(status(**kwargs))
        malformed = bytearray(status(ssid=b"a"))
        malformed[12] = 1
        for report in (malformed, status()[:-1], status() + b"\1", bytes((13,)) + status()[1:]):
            with self.assertRaises(ValueError):
                parse_wireless_status_report(report)
        clear = parse_wireless_status_report(status(flags=0x33, staged=3))
        self.assertTrue(clear["ssid_staged"] and clear["password_staged"])
        self.assertEqual(parse_wireless_status_report(status(staged=8))["staged_state"], 8)

    def test_staging_limits_and_save_switch_mask(self):
        for opcode, value, size in ((6, b"x" * 32, 32), (6, "", 0), (7, "", 0),
                                     (7, "x" * 63, 63), (8, "00" * 32, 32), (8, "", 0)):
            command = build_wireless_command_report(1, opcode, value=value)
            self.assertEqual((len(command), command[3], command[5]), (69, opcode, size))
            self.assertFalse(any(command[6 + size:]))
        for opcode, value in ((6, b"x" * 33), (7, "short"), (7, "a" * 64), (7, "abcde\nxy"),
                              (8, "ab"), (8, b"a" * 31)):
            with self.subTest(opcode=opcode), self.assertRaises(ValueError):
                build_wireless_command_report(1, opcode, value=value)
        for force, commit, flags in ((False, False, 0), (True, False, 1), (False, True, 2), (True, True, 3)):
            command = build_settings_command_report(BOARD, 7, force=force, commit=commit)
            self.assertEqual(command[:9], bytes((20, 1, 7, 5, flags, 200, 0, 1, 7)))
            self.assertEqual(command[9:], bytes(60))
        with self.assertRaisesRegex(ValueError, "saved TCP"):
            build_settings_command_report({key: value for key, value in BOARD.items() if key != "tcp_cli_enabled"})
        validate_wireless_configuration(settings())
        validate_wireless_configuration(settings(ble_key="00" * 32))
        for overrides in ({"confirm_reboot": False}, {"tcp_cli_enabled": 0}, {"password": "short"},
                          {"use_tested_wifi": True, "password": "secret12"}, {"unknown": 1}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                validate_wireless_configuration(settings(**overrides))

    def test_scan_entry_layout_and_security(self):
        for security, label in enumerate(("Open", "WEP", "WPA", "WPA", "RSN (WPA2/WPA3)",
                                          "RSN (WPA2/WPA3)", "WPA/RSN", "WPA/RSN")):
            data = bytearray(63)
            data[:6] = bytes((0x34, 0x12, 2, 2, 0, 255))
            data[36:38] = bytes((214, security))
            parsed = parse_control_result_report(result(12, data=data))["scan_entry"]
            self.assertEqual((parsed["generation"], parsed["index"], parsed["ssid_hex"]),
                             (0x1234, 2, "00ff"))
            self.assertEqual((parsed["rssi"], parsed["security"]), (-42, label))
        for offset, value in ((0, 0), (2, 18), (3, 33), (6, 1), (37, 8), (38, 1)):
            data = bytearray(63)
            data[:6] = bytes((1, 0, 0, 2, 65, 66))
            data[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_control_result_report(result(12, data=data))
        command = build_wireless_command_report(4, 12, index=2, generation=0x1234)
        self.assertEqual(command[5:8], bytes((2, 0x34, 0x12)))

    def test_detail_namespace_and_status_applicability(self):
        valid = [(4, 3, 3, 3), (4, 5, 5, 2), (4, 6, 6, 10), (5, 3, 3, 27),
                 (5, 5, 5, 26), (6, 3, 3, 16), (7, 3, 3, 17), (8, 3, 3, 18),
                 (9, 3, 3, 20), (12, 3, 3, 22), (13, 3, 3, 24), (12, 5, 5, 26)]
        for opcode, state, error, detail in valid:
            parsed = parse_control_result_report(result(opcode, state=state, error=error, data=bytes((detail,))))
            self.assertEqual(parsed["detail"], detail)
        invalid = [(4, 6, 6, 8), (4, 6, 6, 9), (5, 5, 5, 28), (6, 5, 5, 26),
                   (13, 5, 5, 25), (10, 1, 0, 0), (6, 1, 0, 0), (5, 3, 1, 27),
                   (12, 3, 3, 16), (5, 2, 0, 255)]
        for opcode, state, error, detail in invalid:
            with self.subTest(opcode=opcode, detail=detail), self.assertRaises(ValueError):
                parse_control_result_report(result(opcode, state=state, error=error, data=bytes((detail,))))
        with self.assertRaises(ValueError):
            parse_control_result_report(result(6, state=3, error=3, data=b"\x10\x01"))

    def test_test_workflow_wrap_and_independent_generations(self):
        client = self.client([
            status(test=255, generation=900, scan=2),
            result(6, 1), result(7, 2), result(9, 3),
            status(wifi=5, flags=0xB3, test=255, generation=900, scan=2),
            status(wifi=4, flags=0x33, test=1, generation=900, scan=2),
            status(wifi=5, flags=0xB3, test=1, generation=900, scan=2),
        ])
        poll, progress = Mock(), Mock()
        with patch("picoforge.hid.device.sleep") as sleep:
            tested = client.test_wireless(b"Pico\0\xff", "secret12", on_poll=poll, on_progress=progress)
        commands = [bytes(call.args[0]) for call in client._device.send_feature_report.call_args_list]
        self.assertEqual([command[3] for command in commands], [6, 7, 9])
        self.assertEqual(commands[0][6:12], b"Pico\0\xff")
        self.assertEqual(tested, {"ipv4": "192.168.1.2", "detail": 0, "phase": "",
                                  "test_generation": 1, "remaining_seconds": 600})
        self.assertEqual(poll.call_count, 3)
        sleep.assert_called_with(0.5)
        self.assertFalse(client._command_uncertain)

    def test_scan_workflow_and_entry_generation_validation(self):
        data = bytearray(63)
        data[:5] = bytes((1, 0, 0, 1, 65))
        data[36] = 200
        client = self.client([
            status(scan=2, generation=65535, test=22), result(11),
            status(scan=1, generation=1, test=22), status(scan=2, generation=1, count=1, test=22),
            result(12, 2, data=data),
        ])
        with patch("picoforge.hid.device.sleep"):
            networks = client.scan_wireless()
        self.assertEqual(networks[0]["ssid"], "A")
        sent = client._device.send_feature_report.call_args_list
        self.assertEqual([call.args[0][3] for call in sent], [11, 12])
        self.assertEqual(sent[1].args[0][5:8], [0, 1, 0])
        client = self.client([status(), result(11), status(scan=2, generation=1, count=1),
                              result(12, 2, data=bytes((2,)) + data[1:])])
        with self.assertRaisesRegex(ValueError, "generation"):
            client.scan_wireless()
        self.assertFalse(client.command_uncertain)

    def test_test_failure_expiry_and_generation_mismatch(self):
        for final, error in ((status(wifi=6, test=1, flags=0x33, detail=3), RuntimeError),
                             (status(wifi=4, test=2, flags=0x33), ValueError)):
            client = self.client([status(), result(6), result(7, 2), result(9, 3), final])
            with self.assertRaises(error):
                client.test_wireless("new", "secret12")
            self.assertEqual(client.command_uncertain, error is ValueError)
        client = self.client([status(staged=8)])
        with self.assertRaisesRegex(RuntimeError, "expired"):
            client.set_wireless(settings(use_tested_wifi=True))
        client._device.send_feature_report.assert_not_called()

    def test_tested_save_never_restages_pair_or_reads_after_save(self):
        candidate = status(wifi=5, flags=0xB3, test=9)
        saved = bytes((200, 0, 1, 7, 3))
        client = self.client([candidate, candidate, result(5, data=saved)])
        response = client.set_wireless(settings(use_tested_wifi=True))
        sent = client._device.send_feature_report.call_args_list
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0].args[0][3:5], [5, 2])
        self.assertEqual(response["settings"], BOARD)
        self.assertEqual(response["ssid_hex"], "Pico網".encode().hex())
        self.assertTrue(response["stale"])
        self.assertEqual(client._device.get_feature_report.call_count, 3)
        client = self.client([candidate])
        with self.assertRaisesRegex(RuntimeError, "exact staged"):
            client.set_wireless(settings(use_tested_wifi=True, ssid="different"))
        client._device.send_feature_report.assert_not_called()

    def test_sequential_save_stages_only_provided_fields_and_verifies_commit(self):
        staged = status(flags=0x73)
        client = self.client([status(), result(6), result(7, 2), result(8, 3), staged,
                              result(5, 4, data=bytes((200, 0, 1, 7, 7)))])
        response = client.set_wireless(settings(password="secret12", ble_key="ab" * 32))
        self.assertEqual(response["settings"], BOARD)
        self.assertEqual([call.args[0][3] for call in client._device.send_feature_report.call_args_list],
                         [6, 7, 8, 5])
        client = self.client([status(), status(), result(5, data=bytes((200, 0, 1, 7, 0)))])
        request = settings()
        request.pop("ssid")
        client.set_wireless(request)
        self.assertEqual(client._device.send_feature_report.call_count, 1)
        self.assertEqual(client._device.send_feature_report.call_args.args[0][4], 0)
        for data in (bytes((150, 0, 1, 7, 1)), bytes((200, 0, 1, 7, 0))):
            client = self.client([status(), result(6), status(flags=0x13), result(5, 2, data=data)])
            with self.assertRaisesRegex(ProtocolError, "differ"):
                client.set_wireless(settings())
            self.assertFalse(client.command_uncertain)

    def test_hex_save_uses_raw_ssid_and_returns_verified_saved_fields(self):
        board = {**BOARD, "tcp_cli_enabled": False, "radios": 1}
        client = self.client([status(), result(6), result(7, 2), status(flags=0x33),
                              result(5, 3, data=bytes((200, 0, 1, 1, 3)))])
        request = settings(ssid="display-only", ssid_hex="4100fF", password="", ble_enabled=False,
                           tcp_cli_enabled=False, board=board)
        response = client.set_wireless(request)
        sent = client._device.send_feature_report.call_args_list
        self.assertEqual(sent[0].args[0][5:9], [3, 65, 0, 255])
        self.assertEqual(response["ssid_hex"], "4100ff")
        self.assertEqual(response["ssid"], "A\\x00\\xFF")
        self.assertFalse(response["ble_enabled"])
        self.assertFalse(response["tcp_cli_enabled"])
        self.assertEqual(response["settings"], board)
        for encoded in ("0", "gg", "01 02", "a" * 66, None, 12):
            with self.subTest(encoded=encoded), self.assertRaises(ValueError):
                validate_wireless_configuration(settings(ssid_hex=encoded))

    def test_all_zero_control_key_is_present_and_only_empty_input_clears(self):
        for key, configured in (("00" * 32, True), ("", False)):
            client = self.client([status(), result(8), status(flags=0x43, staged=0 if configured else 4),
                                  result(5, 2, data=bytes((200, 0, 1, 7, 4)))])
            request = settings(ble_key=key)
            request.pop("ssid")
            response = client.set_wireless(request)
            self.assertEqual(response["ble_key_configured"], configured)
            staged_command = client._device.send_feature_report.call_args_list[0].args[0]
            self.assertEqual(staged_command[5], 32 if configured else 0)

    def test_cancel_during_scan_uses_opcode_ten_without_clearing_scan(self):
        client = self.client([result(10)])
        client.cancel_wireless_test()
        command = client._device.send_feature_report.call_args.args[0]
        self.assertEqual(command[:5], [20, 1, 1, 10, 0])
        self.assertEqual(command[5:], [0] * 64)
        client._device.get_feature_report.assert_called_once_with(20, 69)

    def test_cancelled_test_sends_cancel_and_leaves_outcome_known(self):
        client = self.client([status(), result(6, 1), result(7, 2), result(9, 3), result(10, 4)])
        stop = threading.Event()
        stop.set()
        with self.assertRaises(WirelessTestCancelled):
            client.test_wireless("Pico", "secret12", cancel=stop)
        sent = [call.args[0][3] for call in client._device.send_feature_report.call_args_list]
        self.assertEqual(sent, [6, 7, 9, 10])
        self.assertFalse(client._command_uncertain)

    def test_failed_cancel_is_known_and_lost_cancel_reconciles_its_own_result_only(self):
        for lost in (False, True):
            with self.subTest(lost=lost):
                client = self.client([
                    status(), result(6, 1), result(7, 2), result(9, 3),
                    OSError("lost cancel reply") if lost else result(10, 4, state=6, error=6),
                ])
                stop = threading.Event()
                stop.set()
                with self.assertRaises((OSError, RuntimeError)):
                    client.test_wireless("Pico", "secret12", cancel=stop)
                self.assertEqual(client.command_uncertain, lost)
                self.assertIsNone(client.uncertain_wireless_operation)
                self.assertEqual(client.uncertain_command, (4, 10) if lost else None)
                if lost:
                    client._device.get_feature_report.side_effect = [result(10, 4)]
                    self.assertIsNotNone(client.reconcile_outcome((4, 10), None))
                    self.assertFalse(client.command_uncertain)
                self.assertEqual(client._device.send_feature_report.call_count, 4)

    def test_timeout_does_not_replay_scan_save_or_proof(self):
        # A stale result cannot resolve a SET, even when its opcode matches.
        for opcode in (5, 11, 13):
            client = self.client([result(opcode, 255)])
            command = (build_settings_command_report(BOARD, 1) if opcode == 5 else
                       build_wireless_command_report(1, opcode, value=bytes(16) if opcode == 13 else None))
            with patch("picoforge.hid.device.monotonic", side_effect=[0, 6]), self.assertRaises(TimeoutError):
                client._execute_control(command, opcode)
            self.assertTrue(client._command_uncertain)
            with self.assertRaisesRegex(RuntimeError, "uncertain"):
                client.cancel_wireless_test()
            client._device.send_feature_report.assert_called_once()

    def test_async_timeout_preserves_uncertainty(self):
        client = self.client([])
        with patch("picoforge.hid.device.monotonic", side_effect=[0, 36]), self.assertRaises(TimeoutError):
            client._poll_wireless("test", 0, 35)
        self.assertTrue(client.command_uncertain)
        self.assertEqual(client.uncertain_wireless_operation, ("test", 1))

    def test_reconcile_requires_matching_command_and_terminal_wireless_generation(self):
        client = self.client()
        client._command_uncertain = True
        client._uncertain_command = (7, 11)
        client._uncertain_wireless_operation = ("scan", 3)
        statuses = iter((status(scan=1, generation=3), status(scan=2, generation=3, count=1)))

        def read_report(report_id, _size):
            if report_id == CONTROL_REPORT_ID:
                return result(11, transaction=7)
            if report_id == WIRELESS_REPORT_ID:
                return next(statuses)
            raise AssertionError(f"Unexpected HID report {report_id}")

        client._device.get_feature_report.side_effect = read_report
        self.assertIsNone(client.reconcile_outcome((8, 11), ("scan", 3)))
        self.assertTrue(client.command_uncertain)
        self.assertIsNone(client.reconcile_outcome((7, 11), ("scan", 3)))
        self.assertTrue(client.command_uncertain)
        control, wireless = client.reconcile_outcome((7, 11), ("scan", 3))
        self.assertEqual((control["transaction_id"], control["opcode"]), (7, 11))
        self.assertEqual(wireless["scan_state"], "complete")
        self.assertFalse(client.command_uncertain)

    def test_commands_cannot_interleave_with_proof_retry(self):
        entered, release, contender = threading.Event(), threading.Event(), threading.Event()
        errors = []
        client = self.client()
        proof = bytes(range(16))
        calls = 0

        def receive(report_id, size):
            nonlocal calls
            calls += 1
            if calls == 1:
                entered.set()
                if not release.wait(2):
                    raise AssertionError("test release timed out")
                return result(13, data=proof)
            return result(10, 2)

        client._device.get_feature_report.side_effect = receive

        def authenticate():
            try:
                client.tcp_cli_auth_proof(bytes(16), bytes.fromhex("0123456789abcdef"))
            except Exception as error:
                errors.append(error)

        def cancel():
            contender.set()
            try:
                client.cancel_wireless_test()
            except Exception as error:
                errors.append(error)

        first = threading.Thread(target=authenticate)
        second = threading.Thread(target=cancel)
        first.start()
        try:
            self.assertTrue(entered.wait(2))
            second.start()
            self.assertTrue(contender.wait(2))
            self.assertEqual(client._device.send_feature_report.call_count, 1)
        finally:
            release.set()
            first.join(2)
            if second.ident is not None:
                second.join(2)
        self.assertFalse(first.is_alive() or second.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual([call.args[0][3] for call in client._device.send_feature_report.call_args_list], [13, 10])

    def test_saved_switches_come_from_report_two_not_runtime(self):
        client = self.client([status()])
        client.configuration = {"radios": 1, "tcp_cli_enabled": False}
        parsed = client.read_wireless()
        self.assertTrue(parsed["wifi_enabled"])
        self.assertFalse(parsed["ble_enabled"])
        self.assertFalse(parsed["tcp_cli_enabled"])
        client.read_configuration = Mock(return_value=client.configuration)
        request = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 1}
        self.assertFalse(client._complete_settings(request)["tcp_cli_enabled"])
        client.read_configuration.side_effect = OSError("no Report 2")
        with self.assertRaises(OSError):
            client._complete_settings(request)

    def test_status_read_returns_owned_snapshot_without_mutating_previous_reads(self):
        for initial, active in ((status(), False), (status(wifi=4, flags=0x33, test=1), True),
                                (status(scan=1, generation=1), True)):
            client = self.client([initial, OSError("USB")])
            cached = client.read_wireless()
            self.assertEqual(client.wireless_active, active)
            with self.assertRaises(OSError):
                client.read_wireless()
            self.assertFalse(cached["stale"])

    def test_proof_retention_get_retry_and_no_set_replay(self):
        nonce, proof = bytes(range(16)), bytes(range(16, 32))
        request = build_tcp_cli_auth_request(7, nonce)
        self.assertEqual((request[:5], request[5:21], request[21:]), (bytes((20, 1, 7, 13, 0)), nonce, bytes(48)))
        parsed = parse_control_result_report(result(13, 7, data=proof))
        self.assertEqual((parsed["transaction_id"], parsed["opcode"], parsed["proof"]), (7, 13, proof))
        stale = parse_control_result_report(result(13, 8, data=proof))
        self.assertNotEqual(stale["transaction_id"], 7)
        client = self.client([OSError("retry GET only"), result(13, data=proof)])
        with patch("picoforge.hid.device.sleep"):
            self.assertEqual(client.tcp_cli_auth_proof(nonce, bytes.fromhex("0123456789abcdef")), proof)
        self.assertEqual(client._device.send_feature_report.call_count, 1)
        self.assertEqual(client._device.get_feature_report.call_count, 2)
        self.assertIsNone(client.last_command_result["proof"])
        self.assertNotIn("result_data", client.last_command_result)

    def test_uncertain_delivery_blocks_all_writes_including_proof(self):
        for failure in ("short", OSError("USB"), ValueError("invalid result")):
            client = self.client([failure])
            if failure == "short":
                client._device.send_feature_report.side_effect = lambda _: 1
            elif isinstance(failure, ValueError):
                client._device.get_feature_report.side_effect = [bytes(69)]
            with self.assertRaises((OSError, ValueError)):
                client.cancel_wireless_test()
            self.assertTrue(client._command_uncertain)
            with self.assertRaisesRegex(RuntimeError, "uncertain"):
                client.tcp_cli_auth_proof(bytes(16), bytes.fromhex("0123456789abcdef"))
            self.assertEqual(client._device.send_feature_report.call_count, 1)

    def test_http_routes_validate_sanitize_and_preserve_raw_ssid(self):
        boards = Mock()
        dashboard = boards.monitor.return_value
        saved = {"wireless": parse_wireless_status_report(status()), "reboot_pending": True, "settings": BOARD}
        dashboard.set_wireless.side_effect = lambda body: validate_wireless_configuration(body) or saved
        dashboard.test_wireless.return_value = {"test": {"state": 5}}
        dashboard.scan_wireless.return_value = {"networks": []}
        dashboard.cancel_wireless_test.return_value = {"cancelled": True}
        client = create_app(boards).test_client()
        for body in (None, [], settings(confirm_reboot=False), settings(password="short")):
            self.assertEqual(client.post("/api/wireless?board=a", json=body).status_code, 400)
        body = settings(password="secret12", ble_key="ab" * 32)
        response = client.post("/api/wireless?board=a", json=body)
        self.assertEqual(response.status_code, 200)
        self.assertNotIn("secret12", response.text)
        self.assertNotIn("ab" * 32, response.text)
        self.assertEqual(client.post("/api/wireless/scan?board=a").status_code, 200)
        self.assertEqual(client.post("/api/wireless/cancel?board=a").json, {"cancelled": True})
        self.assertEqual(client.post("/api/wireless/test?board=a",
                                     json={"ssid": [65, 0, 255], "password": ""}).status_code, 200)
        dashboard.test_wireless.assert_called_once_with([65, 0, 255], "")
        boards.monitor.assert_called_with("a")
        self.assertEqual(client.post("/api/wireless/test?board=a",
                                     json={"ssid": "ignored", "ssid_hex": "4100FF", "password": ""}).status_code, 200)
        dashboard.test_wireless.assert_called_with(b"A\0\xff", "")
        for failure, uncertain in (
            (TimeoutError("secret12"), True),
            (OSError("secret12"), False),
            (ProtocolError("secret12"), False),
        ):
            dashboard.test_wireless.side_effect = failure
            response = client.post("/api/wireless/test", json={"ssid": "Pico", "password": "secret12"})
            self.assertEqual(response.status_code, 503)
            self.assertEqual(response.json["uncertain"], uncertain)
            self.assertNotIn("secret12", response.text)
        dashboard.test_wireless.side_effect = ValueError("bug")
        response = client.post("/api/wireless/test", json={"ssid": "Pico", "password": ""})
        self.assertEqual(response.status_code, 500)
        self.assertEqual(response.json["error"], "Internal server error")

    def monitor(self):
        client = Mock()
        return make_monitor(client, {"connected": True, "hardware": {"wireless_supported": True},
                                     "memory": {"msc": {"enabled": False, "transition_pending": False}}}), client

    def test_busy_command_reply_does_not_block_later_wireless_commands(self):
        monitor, client = self.monitor()
        monitor._snapshot["hardware"] = {"wireless_supported": True, "led_available": True}
        client.set_led.return_value = {"status": "busy", "error": "busy", "detail": 0}
        with self.assertRaisesRegex(RuntimeError, "LED command busy"):
            monitor.set_led(True)
        self.assertEqual(monitor.snapshot()["control_result"]["status"], "busy")
        client.scan_wireless.return_value = []
        self.assertEqual(monitor.scan_wireless(), {"networks": []})

    def test_monitor_ownership_gates_and_input_pump(self):
        monitor, client = self.monitor()
        baseline = monitor._snapshot.copy()
        for change in ({"connected": False}, {"hardware": {}}, {"memory": None},
                       {"memory": {"msc": {"enabled": True}}}, {"memory": {"msc": {"transition_pending": True}}},
                       {"self_test": {"active": True}}, {"reboot_pending": True}):
            monitor._snapshot = {**baseline, **change}
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                monitor.set_wireless(settings())
            client.set_wireless.assert_not_called()
        monitor._snapshot = baseline
        client.read_input_report.return_value = {"kind": "self_test", "data": {"active": False}}
        monitor._drain_input_reports()
        self.assertEqual(client.read_input_report.call_count, 16)
        client.read_input_report.side_effect = OSError("USB disconnected")
        with self.assertRaises(OSError):
            monitor._drain_input_reports()

    def test_monitor_polls_wireless_at_active_and_idle_intervals(self):
        for active, interval in ((False, 2.0), (True, 0.5)):
            monitor, client = self.monitor()
            monitor._stop = threading.Event()
            client.wireless_active = active
            client.read_info.return_value = {
                "supported": True, "settings_supported": False, "wireless_supported": True,
            }
            client.read_configuration.return_value = BOARD
            client.read_input_report.return_value = None
            client.read_wireless.side_effect = [OSError("bad status"), {"ssid": "new", "stale": False}]
            monitor._client_factory = lambda: client
            now = [0.0]
            iterations = []

            def wait(timeout):
                if timeout == 0.05:
                    iterations.append(now[0])
                    now[0] += interval / 2
                    if len(iterations) == 3:
                        monitor._stop.set()
                return False

            monitor._stop.wait = wait
            with patch("picoforge.dashboard.monitor.time.monotonic", side_effect=lambda: now[0]):
                monitor._run()
            self.assertEqual(client.read_wireless.call_count, 2)
            self.assertEqual(monitor._snapshot["wireless"]["ssid"], "new")
            self.assertIsNone(monitor._snapshot["wireless_error"])

    def test_monitor_keeps_serving_inputs_and_freshness_while_testing(self):
        monitor, client = self.monitor()
        runtime = {"temperature_valid": True, "temperature_centidegrees": 2500,
                   "led_available": True, "led_on": False}
        memory = {"msc": {"enabled": False, "transition_pending": False}, "filesystem_format": "fat32"}
        client.read_input_report.side_effect = [
            {"kind": "runtime", "data": runtime}, {"kind": "memory", "data": memory},
            {"kind": "self_test", "data": {"active": False}}, None,
        ]

        def joining(ssid, password, *, on_poll, on_progress, cancel):
            on_progress("Connecting to Wi-Fi")
            self.assertEqual(monitor.snapshot()["wireless_phase"], "Connecting to Wi-Fi")
            monitor._input_received_at = {3: 0, 10: 0, 11: 0}
            with patch("picoforge.dashboard.monitor.time.monotonic", return_value=30):
                self.assertTrue(monitor.snapshot()["telemetry_stale"])
                on_poll()
                snapshot = monitor.snapshot()
                self.assertFalse(snapshot["telemetry_stale"])
                self.assertFalse(snapshot["storage_stale"])
                self.assertEqual(snapshot["runtime"], runtime)
                self.assertEqual(snapshot["memory"], memory)
            return {"remaining_seconds": 600}

        client.test_wireless.side_effect = joining
        self.assertEqual(monitor.test_wireless("Pico網", "secret12")["test"]["remaining_seconds"], 600)
        self.assertIsNone(monitor.snapshot()["wireless_phase"])

    def test_cancel_reaches_running_test_while_other_commands_get_busy(self):
        for finishes_first in (False, True):
            with self.subTest(finishes_first=finishes_first):
                monitor, client = self.monitor()
                monitor._snapshot["self_test"] = {"active": False}
                started = threading.Event()

                def joining(ssid, password, *, on_poll, on_progress, cancel):
                    started.set()
                    self.assertTrue(cancel.wait(10), "never cancelled")
                    if finishes_first:
                        return {"remaining_seconds": 600}
                    client.cancel_wireless_test()
                    raise WirelessTestCancelled("Wi-Fi test cancelled")

                client.test_wireless.side_effect = joining
                outcome = []
                worker = threading.Thread(target=lambda: outcome.append(self.run_test(monitor)))
                worker.start()
                self.assertTrue(started.wait(2))
                with patch("picoforge.dashboard.monitor._BUSY_WAIT_SECONDS", 0.1), \
                        self.assertRaisesRegex(RuntimeError, "busy with another command"):
                    monitor.set_self_test("start", 1)
                client.set_self_test.assert_not_called()
                self.assertEqual(monitor.cancel_wireless_test(), {"cancelled": True})
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(outcome, [600 if finishes_first else "cancelled"])
                client.cancel_wireless_test.assert_called_once_with()
                self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])

    @staticmethod
    def run_test(monitor):
        try:
            return monitor.test_wireless("Pico", "secret12")["test"]["remaining_seconds"]
        except WirelessTestCancelled:
            return "cancelled"

    def test_monitor_proof_uncertainty_propagates_to_other_writes(self):
        monitor, client = self.monitor()
        client.command_uncertain = True
        client.tcp_cli_auth_proof.side_effect = OSError("USB")
        with self.assertRaises(CommandOutcomeUncertain):
            monitor.tcp_cli_auth_proof(bytes(16), bytes(8))
        self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
        with self.assertRaisesRegex(RuntimeError, "uncertain"):
            monitor.cancel_wireless_test()


if __name__ == "__main__":
    unittest.main()
