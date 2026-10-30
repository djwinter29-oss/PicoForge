import struct
import unittest

from picoforge.protocol import (
    build_control_command_report,
    build_led_command_report,
    build_msc_command_report,
    build_self_test_command_report,
    build_settings_command_report,
    normalize_legacy_settings,
    parse_configuration_report,
    parse_control_result_report,
    parse_identity_report,
    parse_memory_report,
    parse_self_test_report,
    parse_status_report,
)


def identity_report(capabilities=0x35, mcu=2, board=2):
    payload = (
        bytes([1]) + struct.pack("<H", 2) + bytes.fromhex("0123456789abcdef")
        + bytes([1, 2, 3, mcu, board, 0, capabilities])
        + struct.pack("<II", 520192, 100000)
    )
    return bytes([1]) + payload


def configuration_report(profile=1, transport="sdio"):
    storage_gpios = bytes([10, 11, 12, 13, 14, 15] if transport == "sdio" else [10, 11, 12, 13, 255, 255])
    payload = bytes([1]) + struct.pack("<H", 200) + bytes([
        profile, 255, 255, 255,
    ]) + storage_gpios + bytes([2, 1, 25, 255])
    return bytes([2]) + payload


def telemetry_report(flags=0x0F, led_flags=1, temperature=2345):
    payload = struct.pack("<BBhQHHIBB", 1, led_flags, temperature, 123456, 1234, 5678,
                          150_000_000, 9, flags)
    return bytes([3]) + payload


def storage_report(flags=0x7F, msc=(0, 0, 0)):
    cid = bytes.fromhex("1b53444341524431211234567801aaff")
    if flags & 0x40:
        sample = bytes([1, 1]) + struct.pack("<IBI", 25_000_000, 1, 4096) + cid
    else:
        sample = bytes(2 + 9 + 16)
    payload = bytes([1, 7, flags]) + sample + bytes([3, *msc])
    return bytes([10]) + payload


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.identity = parse_identity_report(identity_report())

    def test_forced_save_capability_and_confirmation_flag(self):
        info = parse_identity_report(identity_report(capabilities=0x75))
        self.assertTrue(info["capabilities"] & 64)
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1}
        self.assertEqual(build_settings_command_report(settings, 3, force=True),
                         bytes([20, 1, 3, 6, 1, 200, 0, 1, 0, 0]))
        self.assertEqual(build_settings_command_report(settings, 3)[4], 0)
        with self.assertRaises(ValueError):
            build_settings_command_report(settings, force="yes")

    def test_identity_and_board_configuration_decode(self):
        self.assertEqual(self.identity["device_uid"], "0123456789ABCDEF")
        self.assertEqual(self.identity["product_id"], 2)
        self.assertEqual(self.identity["storage_transport"], "sdio")
        config = parse_configuration_report(configuration_report(), self.identity)
        self.assertEqual(config["system_clock_mhz"], 200)
        self.assertEqual(config["sdcard_profile"], 1)
        self.assertEqual((config["sdio_bus_mhz"], config["spi_bus_mhz"]), (50, 0))
        raw = identity_report()
        for framed in (bytes([0]) + raw + bytes(4), raw + bytes(4)):
            self.assertEqual(parse_identity_report(framed)["product_id"], 2)
        with self.assertRaises(ValueError):
            parse_identity_report(raw + bytes(3) + b"\x01")

    def test_identity_and_configuration_reject_invalid_contract_fields(self):
        for invalid in (identity_report(capabilities=0), identity_report(capabilities=0xFD),
                        identity_report(mcu=1, board=2)):
            with self.subTest(report=invalid), self.assertRaises(ValueError):
                parse_identity_report(invalid)
        malformed_reserved = bytearray(identity_report())
        malformed_reserved[17] = 1
        self.assertEqual(parse_identity_report(malformed_reserved)["product_id"], 2)
        unknown_product = bytearray(identity_report())
        unknown_product[2:4] = struct.pack("<H", 0x1234)
        unknown = parse_identity_report(unknown_product)
        self.assertEqual(unknown["product_name"], "Unknown product (0x1234)")
        self.assertFalse(unknown["supported"])
        config = bytearray(configuration_report())
        config[14] |= 0x80
        with self.assertRaisesRegex(ValueError, "flags"):
            parse_configuration_report(config, self.identity)
        config = bytearray(configuration_report())
        config[16] = 255
        with self.assertRaisesRegex(ValueError, "LED"):
            parse_configuration_report(config, self.identity)

    def test_telemetry_decodes_shared_values_and_rejects_invalid_flags_and_values(self):
        result = parse_status_report(telemetry_report(), self.identity)
        self.assertEqual(result["sequence"], 9)
        self.assertEqual(result["temperature_centidegrees"], 2345)
        self.assertEqual(result["cpu_usage_core1_basis_points"], 5678)
        invalid_temperature = telemetry_report(flags=0x0B)
        with self.assertRaisesRegex(ValueError, "temperature"):
            parse_status_report(invalid_temperature, self.identity)
        bad_load = bytes([3]) + struct.pack("<BBhQHHIBB", 1, 0, 0, 1, 10001, 0,
                                            125_000_000, 0, 0)
        with self.assertRaisesRegex(ValueError, "CPU usage"):
            parse_status_report(bad_load)
        with self.assertRaisesRegex(ValueError, "exactly"):
            parse_status_report(telemetry_report() + b"\0")

    def test_storage_snapshot_decodes_independent_sd_and_msc_fields(self):
        data = parse_memory_report(storage_report(), self.identity)
        self.assertTrue(data["sample_valid"])
        self.assertTrue(data["initialized"])
        self.assertEqual(data["mode"], "sdio")
        self.assertEqual(data["capacity_bytes"], 4096 * 512)
        self.assertEqual(data["filesystem_format"], "fat32")
        self.assertEqual(data["manufacturer"]["id"], 0x1B)
        invalid = parse_memory_report(storage_report(flags=1, msc=(0, 0, 0)), self.identity)
        self.assertFalse(invalid["sample_valid"])
        self.assertIsNone(invalid["present"])
        self.assertEqual(invalid["filesystem_format"], "fat32")

    def test_storage_rejects_inconsistent_state_and_transport(self):
        for flags in (0xFF, 0x03, 0x5F):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                parse_memory_report(storage_report(flags=flags), self.identity)
        bad_mode = bytearray(storage_report())
        bad_mode[5] = 2
        with self.assertRaisesRegex(ValueError, "transport"):
            parse_memory_report(bad_mode, self.identity)
        pending = storage_report(flags=1, msc=(0, 1, 0))
        self.assertTrue(parse_memory_report(pending, self.identity)["msc"]["transition_pending"])

    def test_control_commands_use_report_20_and_validate_fields(self):
        self.assertEqual(build_led_command_report(True, 9), bytes([20, 1, 9, 1, 0, 1, 0, 0, 0, 0]))
        self.assertEqual(build_msc_command_report(False, force=True, transaction_id=4),
                         bytes([20, 1, 4, 3, 1, 2, 0, 0, 0, 0]))
        self.assertEqual(build_self_test_command_report("start", 1024, 3),
                         bytes([20, 1, 3, 4, 0, 1, 0, 4, 0, 0]))
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1}
        self.assertEqual(build_settings_command_report(settings, 2),
                         bytes([20, 1, 2, 6, 0, 200, 0, 1, 0, 0]))
        for arguments in (b"\x02\0\0\0\0", b"\0\x01\0\0\0"):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                build_control_command_report(1, 1, arguments=arguments)
        with self.assertRaises(ValueError):
            build_control_command_report(1, 2)
        with self.assertRaises(ValueError):
            build_control_command_report(0, 1)
        with self.assertRaises(ValueError):
            build_settings_command_report({"system_clock_mhz": 100, "sdcard_profile": 0})

    def test_control_result_validates_transaction_status_and_data(self):
        result = parse_control_result_report(bytes([20, 1, 11, 6, 2, 0, 200, 0, 1, 0]))
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["saved_settings"], {"system_clock_mhz": 200, "sdcard_profile": 1})
        for malformed in (
            bytes([20, 1, 0, 6, 2, 0, 0, 0, 0, 0]),
            bytes([20, 1, 11, 6, 3, 5, 0, 0, 0, 0]),
            bytes([20, 1, 11, 3, 4, 4, 0, 0, 0, 0]),
            bytes([20, 1, 11, 6, 2, 0, 0, 0, 2, 0]),
        ):
            with self.subTest(report=malformed), self.assertRaises(ValueError):
                parse_control_result_report(malformed)
        unsupported_led = bytes([20, 1, 11, 1, 4, 4, 0, 0, 0, 0])
        with self.assertRaisesRegex(ValueError, "advertised capabilities"):
            parse_control_result_report(unsupported_led, self.identity)

    def test_legacy_settings_translation_uses_selected_transport(self):
        settings = {"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 50}
        self.assertEqual(normalize_legacy_settings(settings, "sdio"),
                         {"system_clock_mhz": 200, "sdcard_profile": 1})
        self.assertEqual(normalize_legacy_settings(
            {"system_clock_mhz": 0, "sdio_bus_mhz": 0, "spi_bus_mhz": 0}, "spi"
        ), {"system_clock_mhz": 0, "sdcard_profile": 0})


if __name__ == "__main__":
    unittest.main()
