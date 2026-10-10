import struct
import unittest

from picoforge_host.protocol import parse_info_report, parse_status_report


def info_report(mcu=2, clock=150_000_000, name="PicoForge"):
    product = name.encode("ascii").ljust(20, b"\0")
    return bytes([1]) + struct.pack("<BBI", 1, mcu, clock) + product


class ProtocolTests(unittest.TestCase):
    def test_decodes_info_and_product_name(self):
        self.assertEqual(
            parse_info_report(info_report()),
            {
                "version": 1,
                "mcu_id": 2,
                "mcu": "RP2350",
                "system_clock_hz": 150_000_000,
                "product_name": "PicoForge",
            },
        )

    def test_accepts_linux_feature_report_id_prefix_and_zero_padding(self):
        report = bytes([0]) + info_report() + bytes(4)
        self.assertEqual(parse_info_report(report)["product_name"], "PicoForge")

    def test_rejects_bad_info_reports(self):
        for report in (b"", bytes(27), bytes([1, 2]) + bytes(25), info_report(clock=0)):
            with self.subTest(report=report):
                with self.assertRaises(ValueError):
                    parse_info_report(report)

    def test_decodes_live_status_and_flags(self):
        payload = struct.pack("<BBIB", 1, 9, 123_456, 3)
        self.assertEqual(
            parse_status_report(bytes([2]) + payload),
            {
                "version": 1,
                "sequence": 9,
                "uptime_ms": 123_456,
                "usb_mounted": True,
                "cdc_open": True,
            },
        )

    def test_rejects_unknown_status_flags(self):
        with self.assertRaisesRegex(ValueError, "unknown HID status flags"):
            parse_status_report(bytes([2]) + struct.pack("<BBIB", 1, 0, 1, 0x80))


if __name__ == "__main__":
    unittest.main()