import struct
import unittest

from picoforge.protocol import build_self_test_command_report, parse_self_test_report


def report(state=0, flags=0, target=0, written=0, read=0, write_ms=0, read_ms=0,
           mismatches=0, error=0, fs_result=0, request_error=0):
    payload = bytes([1, 7, state, flags]) + struct.pack(
        "<IIIIII", target, written, read, write_ms, read_ms, mismatches
    ) + bytes([error, fs_result, request_error, 0])
    return bytes([11]) + payload


class SelfTestProtocolTests(unittest.TestCase):
    def test_commands_use_control_report_20_and_enforce_action_size(self):
        self.assertEqual(build_self_test_command_report("start", 32, 7),
                         bytes([20, 1, 7, 4, 0, 1, 32, 0, 0, 0]))
        self.assertEqual(build_self_test_command_report("cancel", transaction_id=8),
                         bytes([20, 1, 8, 4, 0, 0, 0, 0, 0, 0]))
        with self.assertRaises(ValueError):
            build_self_test_command_report("cancel", 32)
        for size in (0, 1025, True, 32.5):
            with self.subTest(size=size), self.assertRaises(ValueError):
                build_self_test_command_report("start", size)

    def test_decodes_progress_and_retained_terminal_result(self):
        result = parse_self_test_report(report(6, 2, 32 * 1024 * 1024, 32 * 1024 * 1024,
                                               32 * 1024 * 1024, 16000, 8000))
        self.assertEqual(result["state"], "passed")
        self.assertEqual(result["progress_percent"], 100)
        self.assertEqual(result["write_bytes_per_second"], 2097152)
        self.assertTrue(result["volume_mounted"])

    def test_retains_unknown_enums_but_reports_them_as_unknown(self):
        result = parse_self_test_report(report(200, request_error=12))
        self.assertEqual(result["state"], "unknown (200)")
        self.assertEqual(result["request_error_name"], "unknown (12)")

    def test_rejects_invalid_sizes_reserved_fields_and_state_combinations(self):
        for malformed in (
            report(0, target=1),
            report(1, target=1, written=1),
            report(6, 2, 10, 10, 9),
            report(7, error=1),
            report(0, request_error=0, flags=4),
        ):
            with self.subTest(report=malformed), self.assertRaises(ValueError):
                parse_self_test_report(malformed)
        reserved = bytearray(report())
        reserved[-1] = 1
        with self.assertRaises(ValueError):
            parse_self_test_report(reserved)

    def test_requires_filesystem_result_for_filesystem_io_failure(self):
        with self.assertRaisesRegex(ValueError, "FatFs"):
            parse_self_test_report(report(7, target=1024, error=8))
        result = parse_self_test_report(report(7, target=1024, error=8, fs_result=1))
        self.assertEqual(result["error_name"], "filesystem I/O")


if __name__ == "__main__":
    unittest.main()
