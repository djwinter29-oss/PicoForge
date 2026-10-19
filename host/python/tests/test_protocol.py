import csv
from pathlib import Path
import struct
import unittest

from picoforge.protocol import (
    LED_COMMAND_REPORT_ID,
    MSC_COMMAND_REPORT_ID,
    SETTINGS_REPORT_ID,
    build_settings_command_report,
    build_led_command_report,
    build_msc_command_report,
    parse_info_report,
    parse_memory_report,
    parse_filesystem_report,
    parse_msc_status_report,
    parse_status_report,
    parse_settings_report,
)


def info_report(mcu=2, name="PicoForge", serial="0123456789abcdef"):
    product = name.encode("ascii").ljust(20, b"\0")
    return bytes([1]) + struct.pack("<BB", 2, mcu) + product + serial.encode("ascii") + bytes(4)


class ProtocolTests(unittest.TestCase):
    def test_settings_report_round_trip_and_validation(self):
        settings = {"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 25}
        report = build_settings_command_report(settings)
        self.assertEqual(report, bytes([SETTINGS_REPORT_ID, 1, 200, 0, 50, 25]))
        self.assertEqual(parse_settings_report(report), settings)
        self.assertEqual(parse_settings_report(bytes([0, *report])), settings)
        for key, invalid in (("system_clock_mhz", 100), ("sdio_bus_mhz", 40), ("spi_bus_mhz", True)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                build_settings_command_report({**settings, key: invalid})
        with self.assertRaisesRegex(ValueError, "version"):
            parse_settings_report(bytes([SETTINGS_REPORT_ID, 2, *report[2:]]))

    def test_shared_filesystem_vectors(self):
        fixture = Path(__file__).resolve().parents[3] / "tests/fixtures/hid-filesystem.csv"
        with fixture.open() as source:
            for vector in csv.DictReader(source):
                with self.subTest(vector=vector["name"]):
                    report = bytes.fromhex(vector["hex"])
                    if vector["expected"] == "invalid":
                        with self.assertRaises(ValueError):
                            parse_filesystem_report(report)
                    else:
                        self.assertEqual(parse_filesystem_report(report)["format"], vector["expected"])

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
        payload = (struct.pack("<BBIIBh", 1, 9, 123_456, 150_000_000, 0x1F, 2345)
                   + struct.pack("<HHII", 1234, 5678, 65536, 532480))
        self.assertEqual(
            parse_status_report(bytes([2]) + payload),
            {
                "version": 1,
                "sequence": 9,
                "uptime_ms": 123_456,
                "system_clock_hz": 150_000_000,
                "usb_mounted": True,
                "cdc_open": True,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": True,
                "cpu_usage_core0_basis_points": 1234,
                "cpu_usage_core1_basis_points": 5678,
                "ram_used_bytes": 65536,
                "ram_total_bytes": 532480,
            },
        )

    def test_status_layout_v1_validates_values_and_rejects_other_versions(self):
        base = struct.pack("<BBIIBh", 1, 9, 123456, 250000000, 0, 0)
        for version in (2, 3, 4):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "unsupported"):
                parse_status_report(bytes([2]) + bytes([version]) + base[1:] + struct.pack("<HHII", 1234, 5678, 65536, 532480))
        report = bytes([2]) + base + struct.pack("<HHII", 1234, 5678, 65536, 532480)
        status = parse_status_report(report)
        self.assertEqual(status["cpu_usage_core0_basis_points"], 1234)
        self.assertEqual(status["cpu_usage_core1_basis_points"], 5678)
        self.assertNotIn("cpu_usage_basis_points", status)
        unavailable = bytes([2]) + base + struct.pack("<HHII", 0xffff, 0xffff, 0, 270336)
        self.assertIsNone(parse_status_report(unavailable)["cpu_usage_core0_basis_points"])
        self.assertIsNone(parse_status_report(unavailable)["cpu_usage_core1_basis_points"])
        for malformed in (report[:-1], report + b"\x01",
                          bytes([2]) + base + struct.pack("<HHII", 10001, 0, 1, 2),
                          bytes([2]) + base + struct.pack("<HHII", 0, 10001, 1, 2),
                          bytes([2]) + base + struct.pack("<HHII", 0, 0, 3, 2)):
            with self.subTest(report=malformed), self.assertRaises(ValueError):
                parse_status_report(malformed)

    def test_rejects_unknown_status_flags(self):
        with self.assertRaisesRegex(ValueError, "unknown HID status flags"):
            payload = struct.pack("<BBIIBh", 1, 0, 1, 125_000_000, 0x80, 0)
            parse_status_report(bytes([2]) + payload + struct.pack("<HHII", 0, 0, 1, 2))

    def test_builds_led_command_and_rejects_invalid_dynamic_led_state(self):
        self.assertEqual(build_led_command_report(True), bytes([LED_COMMAND_REPORT_ID, 2, 1]))
        self.assertEqual(build_led_command_report(False), bytes([LED_COMMAND_REPORT_ID, 2, 0]))
        with self.assertRaises(ValueError):
            build_led_command_report(1)
        with self.assertRaisesRegex(ValueError, "without an available LED"):
            payload = struct.pack("<BBIIBh", 1, 0, 1, 125_000_000, 0x10, 0)
            parse_status_report(bytes([2]) + payload + struct.pack("<HHII", 0, 0, 1, 2))

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

    def test_rejects_non_printable_ascii_cid_text(self):
        for offset in (1, 2, 3, 7):
            for value in (0x00, 0x1f, 0x7f, 0x80):
                cid = bytearray(bytes([0x1b]) + b"SD" + b"CARD1" + bytes([0x21]) + bytes(7))
                cid[offset] = value
                payload = struct.pack("<BBBBBII", 2, 7, 0x3f, 1, 2, 25_000_000, 4096) + bytes(cid)
                with self.subTest(offset=offset, value=value), self.assertRaisesRegex(ValueError, "SD CID"):
                    parse_memory_report(bytes([3]) + payload)

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