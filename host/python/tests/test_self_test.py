import struct
import unittest

from picoforge.protocol import build_self_test_command_report, parse_self_test_report


class SelfTestProtocolTests(unittest.TestCase):
    def test_encodes_start_and_cancel(self):
        self.assertEqual(build_self_test_command_report("start", 32), bytes([6, 2, 1, 32, 0, 0, 0]))
        self.assertEqual(build_self_test_command_report("cancel"), bytes([6, 2, 0, 0, 0, 0, 0]))
        for size in (0, 129, True, 32.5):
            with self.assertRaises(ValueError):
                build_self_test_command_report("start", size)

    def test_decodes_progress_and_average_speeds(self):
        report = bytes([6]) + struct.pack("<BBBBIIIIIIBBBB", 2, 7, 6, 0,
            33554432, 33554432, 33554432, 16000, 8000, 0, 0, 0, 0, 0)
        status = parse_self_test_report(report)
        self.assertEqual(status["state"], "passed")
        self.assertEqual(status["progress_percent"], 100)
        self.assertEqual(status["write_bytes_per_second"], 2097152)
        self.assertEqual(status["read_bytes_per_second"], 4194304)

    def test_idle_has_no_fake_speed_and_bad_payloads_are_rejected(self):
        report = bytearray([6, 2] + [0] * 31)
        self.assertIsNone(parse_self_test_report(report)["write_bytes_per_second"])
        report[-1] = 1
        with self.assertRaises(ValueError):
            parse_self_test_report(report)