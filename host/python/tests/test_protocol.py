import struct
import unittest

from picoforge.protocol import (
    BOARD_STATUS_REPORT_ID,
    LED_COMMAND_REPORT_ID,
    build_led_command_report,
    parse_board_status_report,
    parse_info_report,
    parse_status_report,
)


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

    def test_accepts_padded_feature_report_with_standard_report_id(self):
        report = info_report() + bytes(4)
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

    def test_decodes_board_temperature_and_led_feature_report(self):
        payload = struct.pack("<BBh", 1, 0x07, 2345)
        report = bytes([BOARD_STATUS_REPORT_ID]) + payload + bytes(2)
        self.assertEqual(
            parse_board_status_report(report),
            {
                "version": 1,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": True,
            },
        )

    def test_builds_led_command_and_rejects_malformed_board_status(self):
        self.assertEqual(build_led_command_report(True), bytes([LED_COMMAND_REPORT_ID, 1, 1]))
        self.assertEqual(build_led_command_report(False), bytes([LED_COMMAND_REPORT_ID, 1, 0]))
        with self.assertRaises(ValueError):
            build_led_command_report(1)
        with self.assertRaisesRegex(ValueError, "unknown"):
            parse_board_status_report(bytes([BOARD_STATUS_REPORT_ID, 1, 0x80, 0, 0]))
        with self.assertRaisesRegex(ValueError, "without an available LED"):
            parse_board_status_report(bytes([BOARD_STATUS_REPORT_ID, 1, 0x04, 0, 0]))


if __name__ == "__main__":
    unittest.main()