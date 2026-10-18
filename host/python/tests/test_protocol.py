import struct
import unittest

from picoforge.protocol import (
    LED_COMMAND_REPORT_ID,
    MSC_COMMAND_REPORT_ID,
    build_led_command_report,
    build_msc_command_report,
    parse_info_report,
    parse_memory_report,
    parse_filesystem_report,
    parse_msc_status_report,
    parse_status_report,
)


def info_report(mcu=2, name="PicoForge", serial="0123456789abcdef"):
    product = name.encode("ascii").ljust(20, b"\0")
    return bytes([1]) + struct.pack("<BB", 2, mcu) + product + serial.encode("ascii") + bytes(4)


class ProtocolTests(unittest.TestCase):
    def test_rejects_unsupported_memory_versions_and_modes(self):
        report = bytearray(30)
        report[0] = 3
        report[1] = 3
        with self.assertRaisesRegex(ValueError, "unsupported HID memory report version"):
            parse_memory_report(report)
        report[1] = 4
        with self.assertRaisesRegex(ValueError, "unsupported HID memory report version"):
            parse_memory_report(report)
        report[1:6] = bytes([2, 0, 0, 2, 2])
        with self.assertRaisesRegex(ValueError, "unknown HID memory type or mode"):
            parse_memory_report(report)
        report[4:6] = bytes([1, 3])
        with self.assertRaisesRegex(ValueError, "unknown HID memory type or mode"):
            parse_memory_report(report)

    def test_decodes_info_and_product_name(self):
        self.assertEqual(
            parse_info_report(info_report()),
            {
                "version": 2,
                "mcu_id": 2,
                "mcu": "RP2350",
                "product_name": "PicoForge",
                "serial_number": "0123456789abcdef",
                "firmware_version": None,
            },
        )

    def test_decodes_firmware_release_from_identity_layout_three(self):
        report = bytearray(info_report())
        report[1] = 3
        report[39:42] = bytes([0, 1, 0])
        for framed in (report, bytes([0]) + report + bytes(4), report + bytes(4)):
            self.assertEqual(parse_info_report(framed)["firmware_version"], "0.1.0")
        report[-1] = 1
        with self.assertRaises(ValueError):
            parse_info_report(report)
        report[-1] = 0
        report[1] = 4
        with self.assertRaises(ValueError):
            parse_info_report(report)

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
                "cpu_usage_basis_points": None,
                "ram_used_bytes": None,
                "ram_total_bytes": None,
            },
        )

    def test_runtime_layout_three_usage_and_legacy_compatibility(self):
        prefix = bytes([2]) + struct.pack("<BBIIBh", 3, 9, 123456, 250000000, 0, 0)
        report = prefix + struct.pack("<HII", 1234, 65536, 532480)
        status = parse_status_report(report + bytes(8))
        self.assertEqual(status["cpu_usage_basis_points"], 1234)
        self.assertEqual(status["ram_used_bytes"], 65536)
        self.assertEqual(status["ram_total_bytes"], 532480)
        unknown = parse_status_report(prefix + struct.pack("<HII", 0xffff, 0, 270336))
        self.assertIsNone(unknown["cpu_usage_basis_points"])
        self.assertEqual(unknown["ram_used_bytes"], 0)
        for cpu in (0, 10000):
            self.assertEqual(parse_status_report(prefix + struct.pack("<HII", cpu, 270336, 270336))
                             ["cpu_usage_basis_points"], cpu)
        for malformed in (report[:-1], report + b"\x01", prefix + struct.pack("<HII", 10001, 1, 2),
                          prefix + struct.pack("<HII", 0, 3, 2), prefix + struct.pack("<HII", 0, 0, 0)):
            with self.subTest(report=malformed), self.assertRaises(ValueError):
                parse_status_report(malformed)

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

    def test_decodes_memory_report_with_sd_cid(self):
        cid = bytes([0x1b]) + b"SD" + b"CARD1" + bytes([0x21]) + bytes.fromhex("12345678") + bytes([0x01, 0xaa, 0xff])
        payload = struct.pack("<BBBBBII", 2, 7, 0x3f, 1, 2, 25_000_000, 4096) + cid
        self.assertEqual(
            parse_memory_report(bytes([3]) + payload),
            {
                "version": 2,
                "sequence": 7,
                "configured": True,
                "presence_known": True,
                "present": True,
                "initialized": True,
                "component_type": "sd-card",
                "mode": "spi",
                "bus_clock_hz": 25_000_000,
                "block_count": 4096,
                "capacity_bytes": 2_097_152,
                "manufacturer": {
                    "id": 0x1b,
                    "oem_id": "SD",
                    "product_name": "CARD1",
                    "product_revision": "2.1",
                    "serial_number": 0x12345678,
                    "manufacture_year": 2026,
                    "manufacture_month": 10,
                },
            },
        )

    def test_distinguishes_unknown_presence_before_initialization(self):
        payload = struct.pack("<BBBBBII", 2, 0, 0x01, 1, 1, 0, 0) + bytes(16)
        status = parse_memory_report(bytes([3]) + payload)
        self.assertIsNone(status["present"])
        self.assertIsNone(status["capacity_bytes"])
        self.assertIsNone(status["manufacturer"])

    def test_decodes_filesystem_format_report(self):
        self.assertEqual(parse_filesystem_report(bytes([7, 2, 3, 4])),
                         {"version": 2, "sequence": 3, "format": "exfat"})
        self.assertEqual(parse_filesystem_report(bytes([7, 2, 3, 0]))["format"], "unknown")
        with self.assertRaisesRegex(ValueError, "unknown HID filesystem format"):
            parse_filesystem_report(bytes([7, 2, 3, 5]))

    def test_encodes_msc_toggle_and_decodes_status(self):
        self.assertEqual(build_msc_command_report(True), bytes([MSC_COMMAND_REPORT_ID, 2, 1]))
        self.assertEqual(build_msc_command_report(False), bytes([MSC_COMMAND_REPORT_ID, 2, 0]))
        self.assertEqual(parse_msc_status_report(bytes([5, 2, 1, 0, 0])),
                         {"enabled": True, "transition_pending": False, "request_rejected": False,
                          "force_return_supported": False})
        self.assertEqual(parse_msc_status_report(bytes([2, 0, 1, 1])),
                         {"enabled": False, "transition_pending": True, "request_rejected": True,
                          "force_return_supported": False})
        with self.assertRaises(ValueError):
            build_msc_command_report(1)
        self.assertEqual(build_msc_command_report(False, force=True), bytes([5, 3, 2]))
        self.assertTrue(parse_msc_status_report(bytes([5, 3, 1, 0, 0]))["force_return_supported"])
        with self.assertRaises(ValueError):
            build_msc_command_report(True, force=True)


if __name__ == "__main__":
    unittest.main()