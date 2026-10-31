import threading
import time
import unittest
from unittest.mock import Mock, patch

from picoforge.device import PicoForgeHid
from picoforge.monitor import DeviceMonitor
from picoforge.protocol import (
    build_wireless_command_report,
    build_wireless_configuration_report,
    parse_wireless_result_report,
    parse_wireless_status_report,
)
from picoforge.web import create_app
from test_protocol import identity_report
from picoforge.protocol import parse_identity_report


BOARD = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 3}


def settings(**overrides):
    return {"wifi_enabled": True, "ble_enabled": True, "ssid": "Pico網",
            "confirm_reboot": True, "board": BOARD, **overrides}


def status(status_code=2, transaction=1):
    report = bytearray(49)
    report[:6] = bytes([30, 1, 3, status_code, transaction, 2])
    report[6:10] = bytes([192, 168, 1, 2])
    ssid = "Pico網".encode()
    report[10:10 + len(ssid)] = ssid
    report[43:45] = b"\1\1"
    return bytes(report)


def operation_result(state=2, transaction=1, page=0, networks=()):
    report = bytearray(221)
    report[:6] = bytes([35, 1, transaction, state, page, len(networks)])
    for index, (ssid, rssi, secured) in enumerate(networks):
        encoded = ssid.encode("utf-8")
        offset = 7 + index * 35
        report[offset] = len(encoded)
        report[offset + 1:offset + 1 + len(encoded)] = encoded
        report[offset + 33] = rssi & 0xFF
        report[offset + 34] = int(secured)
    return bytes(report)


class WirelessTests(unittest.TestCase):
    def test_dashboard_serves_the_shared_wireless_form_without_a_ble_dashboard(self):
        client = create_app(Mock()).test_client()
        page = client.get("/")
        self.assertEqual(page.status_code, 200)
        self.assertIn('id="wifi-ssid"', page.text)
        self.assertIn('id="wifi-password"', page.text)
        self.assertIn('id="ble-secret"', page.text)
        form = client.get("/webhid/js/wireless-form.mjs")
        self.assertEqual(form.status_code, 200)
        self.assertIn("export function bindWirelessForm", form.text)
        self.assertEqual(client.get("/ble").status_code, 404)

    def test_identity_accepts_wireless_without_relaxing_transport_validation(self):
        self.assertTrue(parse_identity_report(identity_report(capabilities=0xF5))["wireless_supported"])
        self.assertFalse(parse_identity_report(identity_report())["wireless_supported"])
        with self.assertRaises(ValueError):
            parse_identity_report(identity_report(capabilities=0xFD))

    def test_builder_offsets_sizes_and_preserved_credentials(self):
        report = build_wireless_configuration_report(settings(), 255)
        self.assertEqual(len(report), 137)
        self.assertEqual(report[:5], bytes([31, 2, 255, 3, 3]))
        self.assertEqual(report[134:], bytes([200, 0, 1]))
        tested = build_wireless_configuration_report(settings(use_tested_wifi=True))
        self.assertEqual(tested[4], 6)
        self.assertEqual(tested[38:102], bytes(64))
        self.assertEqual(report[5:12], "Pico網".encode())
        self.assertEqual(report[38:134], bytes(96))
        report = build_wireless_configuration_report(settings(password="secret12", ble_key="ab" * 32))
        self.assertEqual(report[4], 0)
        self.assertEqual(report[38:47], b"secret12\0")
        self.assertEqual(report[102:134], b"\xab" * 32)
        self.assertEqual(report[134:], bytes([200, 0, 1]))

    def test_input_rejection_never_echoes_credentials(self):
        for change in (
            {"wifi_enabled": 1}, {"wifi_enabled": False}, {"ble_enabled": None}, {"ble_enabled": False},
            {"ssid": "é" * 17},
            {"ssid": "\0"}, {"ssid": "\ud800"}, {"ssid": ""}, {"password": None},
            {"password": "short"}, {"password": "x" * 64}, {"password": "secret\nvalue"},
            {"password": "secret網"}, {"ble_key": "00" * 32}, {"ble_key": "gg" * 32},
            {"ble_key": None}, {"ble_key": "11"}, {"confirm_reboot": 1}, {"force": True},
            {"board": None}, {"board": {**BOARD, "system_clock_mhz": 175}}, {"board": {**BOARD, "radios": 4}},
        ):
            with self.subTest(change=change), self.assertRaises(ValueError) as caught:
                build_wireless_configuration_report(settings(**change))
            self.assertNotIn("secret", str(caught.exception))
        with self.assertRaises(ValueError):
            build_wireless_configuration_report(settings(), 0)
        with self.assertRaisesRegex(ValueError, "board"):
            build_wireless_configuration_report({key: value for key, value in settings().items() if key != "board"})

    def test_scan_test_command_layout_and_result_validation(self):
        command = build_wireless_command_report(9, 3, ssid="Pico網", password="secret12")
        self.assertEqual(len(command), 102)
        self.assertEqual(command[:7], bytes([34, 1, 3, 9, 0, 7, 8]))
        self.assertEqual(command[7:14], "Pico網".encode())
        self.assertEqual(command[39:47], b"secret12")
        self.assertEqual(build_wireless_command_report(10, 2, page=2)[4], 2)
        for args in ((0, 1), (1, 5), (1, 2, {"page": 3}), (1, 3, {"ssid": "", "password": ""}),
                     (1, 3, {"ssid": "Pico", "password": "short"})):
            transaction, operation, *rest = args
            with self.subTest(args=args), self.assertRaises(ValueError):
                build_wireless_command_report(transaction, operation, **(rest[0] if rest else {}))
        parsed = parse_wireless_result_report(operation_result(networks=[("Pico網", -42, True)]))
        self.assertEqual(parsed["networks"], [{"ssid": "Pico網", "rssi": -42, "secured": True}])
        for offset, value in ((1, 2), (3, 7), (4, 3), (5, 19), (41, 2), (220, 1)):
            malformed = bytearray(operation_result())
            malformed[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_wireless_result_report(malformed)

    def test_wireless_command_terminal_failure_is_known_but_delivery_failure_blocks_replay(self):
        client = self.client([])
        sent = []

        def send(command):
            sent.append(bytes(command))
            return len(command)

        def terminal(report_id, _size):
            return status(0, 0) if report_id == 30 else operation_result(6, 1)

        client._device.send_feature_report.side_effect = send
        client._device.get_feature_report.side_effect = terminal
        with self.assertRaisesRegex(RuntimeError, "Could not connect"):
            client.test_wireless("Pico網", "secret12")
        self.assertFalse(client._command_uncertain)
        self.assertEqual(len(sent), 1)

        client = self.client([])
        client._device.get_feature_report.side_effect = lambda report_id, _size: status(0, 0)
        client._device.send_feature_report.side_effect = OSError("detached")
        with self.assertRaisesRegex(OSError, "outcome is uncertain"):
            client.scan_wireless()
        self.assertTrue(client._command_uncertain)
        with self.assertRaisesRegex(RuntimeError, "uncertain"):
            client.scan_wireless()
        client._device.send_feature_report.assert_called_once()

    def test_status_is_sanitized_and_rejects_malformed_reports(self):
        parsed = parse_wireless_status_report(status())
        self.assertEqual(parsed["ssid"], "Pico網")
        self.assertEqual(parsed["ipv4"], "192.168.1.2")
        self.assertEqual(parsed["wifi_state"], "connected")
        self.assertNotIn("password", parsed)
        self.assertNotIn("ble_key", parsed)
        for offset, value in ((1, 2), (2, 4), (3, 7), (5, 4), (43, 2), (44, 2), (45, 4), (48, 1)):
            malformed = bytearray(status())
            malformed[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_wireless_status_report(malformed)
        for malformed in (status()[:-1], bytes([31]) + status()[1:], status() + b"\1"):
            with self.assertRaises(ValueError):
                parse_wireless_status_report(malformed)

    def client(self, reports):
        client = PicoForgeHid.__new__(PicoForgeHid)
        client.closed = False
        client.identity = {"capabilities": 128}
        client._transaction_id = 0
        client._command_uncertain = False
        client._device = Mock()
        client._device.send_feature_report.side_effect = lambda value: len(value)
        client._device.get_feature_report.side_effect = reports
        client.read_settings = Mock(return_value=dict(BOARD))
        return client

    def test_one_set_poll_matching_transaction_and_terminal_rejections(self):
        client = self.client([status(0, 0), status(2, 255), status(1), status()])
        with patch("picoforge.device.sleep"):
            result = client.set_wireless(settings())
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["settings"], BOARD)
        client._device.send_feature_report.assert_called_once()
        self.assertTrue(all(call.args == (30, 49) for call in client._device.get_feature_report.call_args_list))
        self.assertFalse(client._command_uncertain)
        for code, name in ((3, "invalid"), (4, "unsupported"), (5, "busy"), (6, "failed")):
            client = self.client([status(0, 0), status(code)])
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, name):
                client.set_wireless(settings())
            self.assertFalse(client._command_uncertain)
            client._device.send_feature_report.assert_called_once()

    def test_wireless_scan_paging_and_test_join_are_single_attempts(self):
        client = self.client([])
        sent = []

        def send(command):
            sent.append(bytes(command))
            return len(command)

        def receive(report_id, _size):
            if report_id == 30:
                return status(2, 0)
            operation, transaction = sent[-1][2], sent[-1][3]
            if operation == 1:
                result = bytearray(operation_result(2, transaction))
                result[5] = 1
                return result
            if operation == 2:
                return operation_result(2, transaction, networks=[("Pico網", -42, True)])
            if operation == 3:
                result = bytearray(operation_result(5, transaction))
                result[217:221] = bytes([192, 168, 1, 5])
                return result
            return operation_result(0, transaction)

        client._device.send_feature_report.side_effect = send
        client._device.get_feature_report.side_effect = receive
        with patch("picoforge.device.sleep"):
            self.assertEqual(client.scan_wireless(), [{"ssid": "Pico網", "rssi": -42, "secured": True}])
            self.assertEqual(client.test_wireless("Pico網", "secret12")["ipv4"], "192.168.1.5")
            client.cancel_wireless_test()
        self.assertEqual([command[2] for command in sent], [1, 2, 3, 4])
        self.assertEqual(sent[2][39:47], b"secret12")
        self.assertEqual(client._device.send_feature_report.call_count, 4)

    def test_disconnect_timeout_and_short_write_block_replays(self):
        for reports in ([status(0, 0), OSError("secret12")], [status(0, 0), status(1)]):
            client = self.client(reports)
            with patch("picoforge.device.monotonic", side_effect=[0, 6]), self.assertRaises(OSError):
                client.set_wireless(settings())
            self.assertTrue(client._command_uncertain)
            with self.assertRaisesRegex(RuntimeError, "uncertain"):
                client.set_wireless(settings())
            client._device.send_feature_report.assert_called_once()
        client = self.client([status(0, 0)])
        client._device.send_feature_report.side_effect = None
        client._device.send_feature_report.return_value = 1
        with self.assertRaises(OSError):
            client.set_wireless(settings())
        self.assertTrue(client._command_uncertain)

    def test_completed_mismatch_remains_uncertain_and_never_marks_reboot_pending(self):
        for offset, value in ((2, 1), (2, 2), (10, ord("X")), (43, 0), (44, 0)):
            for credentials in ({}, {"password": "secret12", "ble_key": "ab" * 32}):
                report = bytearray(status())
                report[offset] = value
                client = self.client([status(0, 0), bytes(report)])
                monitor = self.monitor()
                monitor._active_client = client
                with self.subTest(offset=offset, credentials=bool(credentials)):
                    with self.assertRaisesRegex(RuntimeError, "uncertain") as caught:
                        monitor.set_wireless(settings(**credentials))
                    self.assertNotIn("secret12", str(caught.exception))
                    self.assertTrue(client._command_uncertain)
                    self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
                    self.assertFalse(monitor.snapshot().get("reboot_pending", False))
                    with self.assertRaisesRegex(RuntimeError, "uncertain"):
                        monitor.set_wireless(settings())
                    client._device.send_feature_report.assert_called_once()

    def test_board_settings_readback_mismatch_remains_uncertain(self):
        client = self.client([status(0, 0), status()])
        client.read_settings.return_value = {**BOARD, "radios": 0}
        with self.assertRaisesRegex(RuntimeError, "uncertain"):
            client.set_wireless(settings())
        self.assertTrue(client._command_uncertain)

    def test_omitted_credentials_must_preserve_absent_configuration(self):
        before = bytearray(status(0, 0))
        before[43:45] = bytes(2)
        for offset in (43, 44):
            after = bytearray(before)
            after[3:5] = bytes([2, 1])
            after[offset] = 1
            client = self.client([bytes(before), bytes(after)])
            with self.subTest(offset=offset), self.assertRaisesRegex(RuntimeError, "uncertain"):
                client.set_wireless(settings())
            self.assertTrue(client._command_uncertain)

    def monitor(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._active_client = Mock()
        monitor._active_client.set_wireless.return_value = {**parse_wireless_status_report(status()),
                                                            "settings": BOARD}
        monitor._snapshot = {"connected": True, "hardware": {"wireless_supported": True},
                             "msc": {"enabled": False, "transition_pending": False}, "storage_stale": False}
        monitor._input_received_at = {10: time.monotonic()}
        monitor._active_client.scan_wireless.return_value = [{"ssid": "Pico網", "rssi": -42, "secured": True}]
        monitor._active_client.test_wireless.return_value = {
            "transaction_id": 1, "state": 5, "page": 0, "count": 0, "wifi_state": 3,
            "ipv4": "192.168.1.5", "networks": [],
        }
        monitor._active_client.cancel_wireless_test.return_value = None
        return monitor

    def test_monitor_ownership_capability_busy_and_uncertainty_gates(self):
        monitor = self.monitor()
        baseline = monitor._snapshot.copy()
        for change in (
            {"connected": False}, {"hardware": {}}, {"msc": None},
            {"msc": {"enabled": True}}, {"msc": {"transition_pending": True}},
            {"self_test": {"active": True}}, {"reboot_pending": True},
            {"control_result": {"status": "pending"}}, {"control_result": {"status": "busy"}},
            {"hardware": {"wireless_supported": True, "self_test_supported": True}},
            {"hardware": {"wireless_supported": True, "self_test_supported": True},
             "self_test": {"active": False}, "self_test_stale": True},
        ):
            monitor._snapshot = {**baseline, **change}
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                monitor.set_wireless(settings())
            monitor._active_client.set_wireless.assert_not_called()
        monitor._input_received_at[10] = time.monotonic() - 2
        with self.assertRaises(RuntimeError):
            monitor.scan_wireless()
        monitor._snapshot = baseline
        monitor._input_received_at[10] = time.monotonic()
        monitor._command_outcome_uncertain = True
        with self.assertRaises(RuntimeError):
            monitor.set_wireless(settings())
        monitor._command_outcome_uncertain = False
        self.assertEqual(monitor.scan_wireless()["networks"][0]["ssid"], "Pico網")
        self.assertEqual(monitor.test_wireless("Pico網", "secret12")["test"]["state"], 5)
        self.assertTrue(monitor.cancel_wireless_test()["cancelled"])
        result = monitor.set_wireless(settings())
        self.assertTrue(result["reboot_pending"])
        self.assertEqual(result["settings"], BOARD)
        self.assertNotIn("settings", result["wireless"])
        self.assertTrue(monitor.snapshot()["reboot_pending"])
        self.assertEqual(monitor.snapshot()["settings"], BOARD)

    def test_http_validation_routing_sanitization_and_uncertain_error(self):
        dashboard = Mock()
        dashboard.set_wireless.return_value = {"wireless": parse_wireless_status_report(status()),
                                               "reboot_pending": True}
        dashboard.read_wireless.return_value = {**dashboard.set_wireless.return_value, "reboot_pending": False}
        dashboard.scan_wireless.return_value = {"networks": [{"ssid": "Pico網", "rssi": -42, "secured": True}]}
        dashboard.test_wireless.return_value = {"test": {"state": 5, "ipv4": "192.168.1.5"}}
        dashboard.cancel_wireless_test.return_value = {"cancelled": True}
        client = create_app(dashboard).test_client()
        for body in (None, [], settings(confirm_reboot=False), settings(password="secret\n12")):
            response = client.post("/api/wireless?board=a", json=body)
            self.assertEqual(response.status_code, 400)
            self.assertNotIn("secret", response.text)
        dashboard.set_wireless.assert_not_called()
        response = client.post("/api/wireless?board=a", json=settings(password="secret12", ble_key="ab" * 32))
        self.assertEqual(response.status_code, 200)
        self.assertNotIn("secret12", response.text)
        self.assertNotIn("ab" * 32, response.text)
        dashboard.set_wireless.assert_called_once()
        self.assertEqual(dashboard.set_wireless.call_args.args[0], "a")
        self.assertEqual(client.get("/api/wireless?board=a").status_code, 200)
        dashboard.read_wireless.assert_called_once_with("a")
        self.assertEqual(client.post("/api/wireless/scan?board=a").json["networks"][0]["ssid"], "Pico網")
        self.assertEqual(client.post("/api/wireless/test?board=a", json={"ssid": "Pico網",
            "password": "secret12"}).json["test"]["state"], 5)
        self.assertNotIn("secret12", client.post("/api/wireless/test?board=a", json={"ssid": "Pico網",
            "password": "secret12"}).text)
        self.assertEqual(client.post("/api/wireless/cancel?board=a").json["cancelled"], True)
        self.assertEqual(client.post("/api/wireless/test", json={"ssid": "Pico", "password": "short"})
                         .status_code, 400)
        dashboard.set_wireless.side_effect = OSError("secret12")
        response = client.post("/api/wireless?board=a", json=settings())
        self.assertEqual(response.status_code, 503)
        self.assertIn("uncertain", response.text)
        self.assertNotIn("secret12", response.text)
