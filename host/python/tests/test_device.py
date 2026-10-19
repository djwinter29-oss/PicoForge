import struct
import unittest
from unittest.mock import patch

from picoforge.device import PicoForgeHid, _matches_vendor_collection
from picoforge.protocol import SELF_TEST_INPUT_SIZE, parse_status_report


class HidEnumerationTests(unittest.TestCase):
    def test_matches_expected_vendor_usage(self):
        self.assertTrue(_matches_vendor_collection({"usage_page": 0xFF00, "usage": 1}))

    def test_rejects_other_vendor_usage(self):
        self.assertFalse(_matches_vendor_collection({"usage_page": 0x0001, "usage": 1}))

    def test_falls_back_when_hidapi_metadata_is_unavailable(self):
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": ""}
            )
        )

    def test_uses_product_string_when_usage_is_unavailable(self):
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "PicoForge CDC + HID"}
            )
        )
        self.assertFalse(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "Other HID device"}
            )
        )
        self.assertTrue(
            _matches_vendor_collection(
                {"usage_page": 0, "usage": 0, "product_string": "PicoForge CDC + HID + MSC"}
            )
        )


class StatusReadTests(unittest.TestCase):
    status = (bytes([2]) + struct.pack("<BBIIBh", 1, 0, 0, 150_000_000, 0, 0)
              + struct.pack("<HHII", 0, 0, 0, 270336))
    non_status = (
        bytes([3, 2]) + bytes(28),
        bytes([5, 2, 0, 0, 0]),
        bytes([6, 2]) + bytes(31),
        bytes([7, 2, 0, 0]),
    )

    def connection(self, reports):
        class FakeTransport:
            def __init__(self):
                self.calls = []

            def read(self, size, timeout):
                self.calls.append((size, timeout))
                return reports.pop(0)[:size] if reports else []

        connection = PicoForgeHid.__new__(PicoForgeHid)
        connection._device = FakeTransport()
        connection.closed = False
        return connection

    def test_skips_supported_reports_before_status(self):
        for report in self.non_status:
            with self.subTest(report_id=report[0]):
                connection = self.connection([report, self.status])
                with patch("picoforge.device.monotonic", return_value=0, create=True):
                    self.assertEqual(connection.read_status(), parse_status_report(self.status))
                self.assertTrue(all(size >= SELF_TEST_INPUT_SIZE for size, _ in connection._device.calls))

    def test_only_non_status_until_deadline(self):
        connection = self.connection(list(self.non_status))
        with patch("picoforge.device.monotonic", side_effect=[0, 0.1, 0.2, 0.25], create=True):
            self.assertIsNone(connection.read_status(250))
        self.assertEqual([timeout for _, timeout in connection._device.calls], [250, 150, 50])

    def test_no_report(self):
        connection = self.connection([])
        self.assertIsNone(connection.read_status())
        self.assertEqual(len(connection._device.calls), 1)

    def test_non_status_then_empty_read(self):
        connection = self.connection([self.non_status[0]])
        self.assertIsNone(connection.read_status())
        self.assertEqual(len(connection._device.calls), 2)


class SettingsDeviceTests(unittest.TestCase):
    def connection(self, report):
        class FakeTransport:
            def get_feature_report(self, report_id, size):
                self.read_request = (report_id, size)
                return report

            def send_feature_report(self, payload):
                self.sent = bytes(payload)
                return len(payload)

        connection = PicoForgeHid.__new__(PicoForgeHid)
        connection._device = FakeTransport()
        connection.closed = False
        return connection

    def test_reads_and_saves_settings_after_confirming_readback(self):
        connection = self.connection(bytes([8, 1, 0, 0, 0, 0]))
        self.assertEqual(connection.read_settings(), {
            "system_clock_mhz": 0, "sdio_bus_mhz": 0, "spi_bus_mhz": 0,
        })
        settings = {"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 25}
        connection._device.get_feature_report = lambda _report_id, _size: bytes([8, 1, 200, 0, 50, 25])
        self.assertEqual(connection.set_settings(settings), settings)
        self.assertEqual(connection._device.sent, bytes([8, 1, 200, 0, 50, 25]))

    def test_rejects_unconfirmed_settings_readback(self):
        connection = self.connection(bytes([8, 1, 0, 0, 0, 0]))
        with self.assertRaisesRegex(RuntimeError, "did not confirm"):
            connection.set_settings({"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 25})


if __name__ == "__main__":
    unittest.main()