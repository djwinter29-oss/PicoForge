import struct
import unittest
from unittest.mock import patch

from picoforge.device import PicoForgeHid, _matches_vendor_collection, enumerate_devices
from picoforge.protocol import (
    CONTROL_FEATURE_SIZE,
    SELF_TEST_INPUT_SIZE,
    parse_identity_report,
    parse_status_report,
)


def identity():
    report = bytes([1, 1]) + struct.pack("<H", 2) + bytes.fromhex("0123456789abcdef")
    report += bytes([1, 2, 3, 2, 2, 0, 0x35]) + struct.pack("<II", 520192, 100000)
    return parse_identity_report(report)


def telemetry():
    return bytes([3]) + struct.pack("<BBhQHHIBB", 1, 0, 0, 42, 0xFFFF, 0xFFFF,
                                    125_000_000, 7, 0x0C)


def control_result(opcode=1, status=2, error=0, transaction=1):
    data = struct.pack("<HBB", 200, 1, 0) if opcode == 6 and status == 2 else bytes(4)
    return bytes([20, 1, transaction, opcode, status, error]) + data


def configuration(clock=200, profile=1):
    payload = bytes([1]) + struct.pack("<H", clock) + bytes([
        profile, 255, 255, 255, 10, 11, 12, 13, 14, 15, 2, 1, 25, 255,
    ])
    return bytes([2]) + payload


class HidEnumerationTests(unittest.TestCase):
    def test_enumerates_board_identity_without_opening_hid(self):
        devices = [
            {"product_string": "PicoForge CDC + HID", "serial_number": "board-A", "path": b"/hid/a"},
            {"product_string": "", "serial_number": "", "path": b"/hid/b"},
        ]
        with patch("picoforge.device._matching_devices", return_value=devices):
            self.assertEqual(enumerate_devices(), [
                {"product_name": "PicoForge CDC + HID", "serial_number": "board-A", "device_path": "/hid/a"},
                {"product_name": "PicoForge", "serial_number": None, "device_path": "/hid/b"},
            ])

    def test_matches_expected_vendor_usage_and_fallback_metadata(self):
        self.assertTrue(_matches_vendor_collection({"usage_page": 0xFF00, "usage": 1}))
        self.assertFalse(_matches_vendor_collection({"usage_page": 1, "usage": 1}))
        self.assertTrue(_matches_vendor_collection({"usage_page": 0, "usage": 0, "product_string": ""}))
        self.assertFalse(_matches_vendor_collection({"usage_page": 0, "usage": 0, "product_string": "Other"}))


class HidDeviceTests(unittest.TestCase):
    def connection(self, reports=None):
        class FakeTransport:
            def __init__(self):
                self.reports = list(reports or [])
                self.sent = []
                self.feature_reports = {}

            def read(self, size, timeout):
                report = self.reports.pop(0) if self.reports else []
                return report[:size]

            def send_feature_report(self, payload):
                self.sent.append(bytes(payload))
                return len(payload)

            def get_feature_report(self, report_id, size):
                self.last_get = (report_id, size)
                response = self.feature_reports[report_id].pop(0)
                return response

        client = PicoForgeHid.__new__(PicoForgeHid)
        client._device = FakeTransport()
        client.closed = False
        client.identity = identity()
        client._transaction_id = 0
        client.last_command_result = None
        client._command_uncertain = False
        return client

    def test_read_status_skips_new_storage_report(self):
        client = self.connection([bytes([10, 1, 0, 1]) + bytes(31), telemetry()])
        with patch("picoforge.device.monotonic", side_effect=[0, 0.1]):
            result = client.read_status(250)
        self.assertEqual(result, parse_status_report(telemetry(), client.identity))

    def test_input_reports_dispatch_under_new_ids(self):
        client = self.connection([telemetry()])
        self.assertEqual(client.read_input_report()["kind"], "runtime")
        self.assertTrue(client._device.reports == [])

    def test_feature_20_command_write_then_result_read(self):
        client = self.connection()
        client._device.feature_reports[20] = [control_result()]
        result = client.set_led(True)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(client._device.sent, [bytes([20, 1, 1, 1, 0, 1, 0, 0, 0, 0])])
        self.assertEqual(client._device.last_get, (20, CONTROL_FEATURE_SIZE))

    def test_settings_save_uses_report_20_and_returns_canonical_readback_without_reboot(self):
        client = self.connection()
        client._device.feature_reports[20] = [control_result(opcode=6)]
        client._device.feature_reports[2] = [configuration()]
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1}
        self.assertEqual(client.set_settings(settings), settings)
        self.assertEqual(client._device.sent, [bytes([20, 1, 1, 6, 0, 200, 0, 1, 0, 0])])
        self.assertEqual(client._device.last_get, (2, 18))

    def test_unavailable_capability_blocks_control_write(self):
        client = self.connection()
        client.identity["capabilities"] = 0x30
        with self.assertRaisesRegex(RuntimeError, "LED"):
            client.set_led(True)
        self.assertEqual(client._device.sent, [])

    def test_command_result_must_match_transaction_and_opcode(self):
        client = self.connection()
        client._device.feature_reports[20] = [control_result(opcode=3, transaction=1)]
        with patch("picoforge.device.monotonic", side_effect=[0, 5.1]):
            with self.assertRaisesRegex(TimeoutError, "uncertain"):
                client.set_led(True)
        with self.assertRaisesRegex(RuntimeError, "uncertain outcome"):
            client.set_led(False)
        self.assertEqual(len(client._device.sent), 1)


if __name__ == "__main__":
    unittest.main()
