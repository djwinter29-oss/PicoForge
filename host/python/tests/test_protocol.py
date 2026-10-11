import struct
import unittest

from picoforge.protocol import (
    LED_COMMAND_REPORT_ID,
    build_led_command_report,
    parse_info_report,
    parse_status_report,
)


def info_report(mcu=2, name="PicoForge", serial="0123456789abcdef"):
    product = name.encode("ascii").ljust(20, b"\0")
    return bytes([1]) + struct.pack("<BB", 2, mcu) + product + serial.encode("ascii") + bytes(4)


class ProtocolTests(unittest.TestCase):
    def test_decodes_info_and_product_name(self):
        self.assertEqual(
            parse_info_report(info_report()),
            {
                "version": 2,
                "mcu_id": 2,
                "mcu": "RP2350",
                "product_name": "PicoForge",
                "serial_number": "0123456789abcdef",
            },
        )

    def test_accepts_linux_feature_report_id_prefix_and_zero_padding(self):
        report = bytes([0]) + info_report() + bytes(4)
        self.assertEqual(parse_info_report(report)["product_name"], "PicoForge")

    def test_accepts_padded_feature_report_with_standard_report_id(self):
        report = info_report() + bytes(4)
        self.assertEqual(parse_info_report(report)["product_name"], "PicoForge")

    def test_rejects_bad_info_reports(self):
        malformed_reserved = bytearray(info_report())
        malformed_reserved[-1] = 1
        for report in (b"", bytes(43), bytes([1, 1]) + bytes(41), malformed_reserved,
                       info_report(serial="not-a-valid-id")):
            with self.subTest(report=report):
                with self.assertRaises(ValueError):
                    parse_info_report(report)

    def test_decodes_live_status_and_flags(self):
        payload = struct.pack("<BBIIBh", 2, 9, 123_456, 150_000_000, 0x1F, 2345)
        self.assertEqual(
            parse_status_report(bytes([2]) + payload),
            {
                "version": 2,
                "sequence": 9,
                "uptime_ms": 123_456,
                "system_clock_hz": 150_000_000,
                "usb_mounted": True,
                "cdc_open": True,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": True,
            },
        )

    def test_rejects_unknown_status_flags(self):
        with self.assertRaisesRegex(ValueError, "unknown HID status flags"):
            parse_status_report(bytes([2]) + struct.pack("<BBIIBh", 2, 0, 1, 125_000_000, 0x80, 0))

    def test_builds_led_command_and_rejects_invalid_dynamic_led_state(self):
        self.assertEqual(build_led_command_report(True), bytes([LED_COMMAND_REPORT_ID, 2, 1]))
        self.assertEqual(build_led_command_report(False), bytes([LED_COMMAND_REPORT_ID, 2, 0]))
        with self.assertRaises(ValueError):
            build_led_command_report(1)
        with self.assertRaisesRegex(ValueError, "without an available LED"):
            parse_status_report(bytes([2]) + struct.pack("<BBIIBh", 2, 0, 1, 125_000_000, 0x10, 0))


if __name__ == "__main__":
    unittest.main()