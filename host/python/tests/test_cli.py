import unittest
from unittest.mock import Mock, patch

from picoforge.cli import _read_status_reports


class StatusCommandTests(unittest.TestCase):
    def test_report_collection_uses_one_overall_deadline(self):
        runtime = {"uptime_ms": 1}
        device = Mock()
        device.read_input_report.return_value = {"kind": "runtime", "data": runtime}

        with patch("picoforge.cli.monotonic", side_effect=[0, 0, 1, 2, 3, 4, 5]):
            result = _read_status_reports(device, self_test_expected=True)

        self.assertEqual(result, (runtime, None, None))
        self.assertEqual(device.read_input_report.call_count, 5)
        self.assertEqual(
            [call.kwargs["timeout_ms"] for call in device.read_input_report.call_args_list],
            [5000, 4000, 3000, 2000, 1000],
        )

    def test_no_reports_returns_after_the_bounded_read(self):
        device = Mock()
        device.read_input_report.return_value = None

        with patch("picoforge.cli.monotonic", side_effect=[10.0, 10.0]):
            result = _read_status_reports(device, self_test_expected=False)

        self.assertEqual(result, (None, None, None))
        device.read_input_report.assert_called_once_with(timeout_ms=5000)


if __name__ == "__main__":
    unittest.main()
