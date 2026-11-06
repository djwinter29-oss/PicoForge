# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

import json
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest.mock import MagicMock, Mock, patch

from picoforge.cli import _read_status_reports, main


class StatusCommandTests(unittest.TestCase):
    def test_module_entry_point_exposes_the_cli_without_hardware(self):
        result = subprocess.run(
            [sys.executable, "-m", "picoforge", "--help"], capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("{status,web,reboot}", result.stdout)

    def test_status_keeps_settings_and_board_configuration_output(self):
        configuration = {
            "system_clock_mhz": 200,
            "sdcard_profile": 1,
            "radios": 3,
            "tcp_cli_enabled": True,
            "reset_cause": 1,
        }
        memory = {"msc": {"enabled": False, "transition_pending": False, "request_rejected": False}}
        device = MagicMock()
        device.__enter__.return_value = device
        device.read_info.return_value = {"supported": True, "self_test_supported": False}
        device.read_configuration.return_value = configuration
        device.read_settings.side_effect = AssertionError("read_settings was removed")
        output = StringIO()
        with patch("picoforge.cli.PicoForgeHid", return_value=device), \
                patch("picoforge.cli._read_status_reports", return_value=(None, memory, None)), \
                redirect_stdout(output):
            self.assertEqual(main(["status"]), 0)

        payload = json.loads(output.getvalue())
        self.assertEqual(payload["settings"], {
            "system_clock_mhz": 200,
            "sdcard_profile": 1,
            "radios": 3,
            "tcp_cli_enabled": True,
        })
        self.assertEqual(payload["board_configuration"], configuration)
        self.assertEqual(payload["msc"], memory["msc"])
        device.read_configuration.assert_called_once_with()
        device.read_settings.assert_not_called()

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
