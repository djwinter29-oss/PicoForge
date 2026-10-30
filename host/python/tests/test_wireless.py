import threading
import unittest
from unittest.mock import Mock, patch

from picoforge.device import PicoForgeHid
from picoforge.monitor import DeviceMonitor
from picoforge.protocol import build_wireless_configuration_report, parse_wireless_status_report
from picoforge.web import create_app
from test_protocol import identity_report
from picoforge.protocol import parse_identity_report


def settings(**overrides):
    return {"wifi_enabled": True, "ble_enabled": True, "ssid": "Pico網",
            "confirm_reboot": True, **overrides}


def status(status_code=2, transaction=1):
    report = bytearray(49)
    report[:6] = bytes([30, 1, 3, status_code, transaction, 2])
    report[6:10] = bytes([192, 168, 1, 2])
    ssid = "Pico網".encode()
    report[10:10 + len(ssid)] = ssid
    report[43:45] = b"\1\1"
    return bytes(report)


class WirelessTests(unittest.TestCase):
    def test_identity_accepts_wireless_without_relaxing_transport_validation(self):
        self.assertTrue(parse_identity_report(identity_report(capabilities=0xF5))["wireless_supported"])
        self.assertFalse(parse_identity_report(identity_report())["wireless_supported"])
        with self.assertRaises(ValueError):
            parse_identity_report(identity_report(capabilities=0xFD))

    def test_builder_offsets_sizes_and_preserved_credentials(self):
        report = build_wireless_configuration_report(settings(), 255)
        self.assertEqual(len(report), 137)
        self.assertEqual(report[:5], bytes([31, 1, 255, 3, 3]))
        self.assertEqual(report[5:12], "Pico網".encode())
        self.assertEqual(report[38:], bytes(99))
        report = build_wireless_configuration_report(settings(password="secret12", ble_key="ab" * 32))
        self.assertEqual(report[4], 0)
        self.assertEqual(report[38:47], b"secret12\0")
        self.assertEqual(report[102:134], b"\xab" * 32)
        self.assertEqual(report[134:], bytes(3))

    def test_input_rejection_never_echoes_credentials(self):
        for change in (
            {"wifi_enabled": 1}, {"ble_enabled": None}, {"ssid": "é" * 17},
            {"ssid": "\0"}, {"ssid": "\ud800"}, {"ssid": ""}, {"password": None},
            {"password": "short"}, {"password": "x" * 64}, {"password": "secret\nvalue"},
            {"password": "secret網"}, {"ble_key": "00" * 32}, {"ble_key": "gg" * 32},
            {"ble_key": None}, {"ble_key": "11"}, {"confirm_reboot": 1}, {"force": True},
        ):
            with self.subTest(change=change), self.assertRaises(ValueError) as caught:
                build_wireless_configuration_report(settings(**change))
            self.assertNotIn("secret", str(caught.exception))
        with self.assertRaises(ValueError):
            build_wireless_configuration_report(settings(), 0)

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
        return client

    def test_one_set_poll_matching_transaction_and_terminal_rejections(self):
        client = self.client([status(0, 0), status(2, 255), status(1), status()])
        with patch("picoforge.device.sleep"):
            self.assertEqual(client.set_wireless(settings())["status"], "completed")
        client._device.send_feature_report.assert_called_once()
        self.assertTrue(all(call.args == (30, 49) for call in client._device.get_feature_report.call_args_list))
        self.assertFalse(client._command_uncertain)
        for code, name in ((3, "invalid"), (4, "unsupported"), (5, "busy"), (6, "failed")):
            client = self.client([status(0, 0), status(code)])
            with self.subTest(code=code), self.assertRaisesRegex(RuntimeError, name):
                client.set_wireless(settings())
            self.assertFalse(client._command_uncertain)
            client._device.send_feature_report.assert_called_once()

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

    def monitor(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._active_client = Mock()
        monitor._active_client.set_wireless.return_value = parse_wireless_status_report(status())
        monitor._snapshot = {"connected": True, "hardware": {"wireless_supported": True},
                             "msc": {"enabled": False, "transition_pending": False}}
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
        ):
            monitor._snapshot = {**baseline, **change}
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                monitor.set_wireless(settings())
            monitor._active_client.set_wireless.assert_not_called()
        monitor._snapshot = baseline
        monitor._command_outcome_uncertain = True
        with self.assertRaises(RuntimeError):
            monitor.set_wireless(settings())
        monitor._command_outcome_uncertain = False
        self.assertTrue(monitor.set_wireless(settings())["reboot_pending"])
        self.assertTrue(monitor.snapshot()["reboot_pending"])

    def test_http_validation_routing_sanitization_and_uncertain_error(self):
        dashboard = Mock()
        dashboard.set_wireless.return_value = {"wireless": parse_wireless_status_report(status()),
                                               "reboot_pending": True}
        dashboard.read_wireless.return_value = {**dashboard.set_wireless.return_value, "reboot_pending": False}
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
        dashboard.set_wireless.side_effect = OSError("secret12")
        response = client.post("/api/wireless?board=a", json=settings())
        self.assertEqual(response.status_code, 503)
        self.assertIn("uncertain", response.text)
        self.assertNotIn("secret12", response.text)
