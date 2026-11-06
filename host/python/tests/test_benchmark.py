# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Keep the documented benchmark workload executable and its measurements honest."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import MagicMock, Mock, patch


SCRIPT = Path(__file__).resolve().parents[3] / "tools/validation/bench-python-dashboard.py"
spec = importlib.util.spec_from_file_location("benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkTests(unittest.TestCase):
    def test_documented_hardware_free_entry_point_has_no_worker_failures(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--seconds", "0.01"],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn("snapshot() copy", result.stdout)
        self.assertIn("GET /api/status", result.stdout)
        self.assertIn("idle poll loop (mocked HID)", result.stdout)

    def test_failed_worker_invalidates_cpu_measurement(self):
        monitor = Mock()
        monitor.snapshot.return_value = {"connected": True, "error": "Internal monitor error"}
        with self.assertRaisesRegex(RuntimeError, "Internal monitor error"):
            benchmark.wait_ready(monitor)

    def test_board_mode_closes_monitor_before_opening_exclusive_read_only_client(self):
        monitor = Mock()
        monitor.snapshot.return_value = benchmark.SNAPSHOT
        device = MagicMock()
        with patch.object(benchmark, "DeviceMonitor", return_value=monitor), \
                patch.object(benchmark, "PicoForgeHid", return_value=device) as factory, \
                patch.object(benchmark, "cpu_ms_per_second", return_value=0.1), \
                patch.object(benchmark, "per_call_us", return_value=100) as measure:
            factory.side_effect = lambda _serial: (monitor.close.assert_called_once(), device)[1]
            benchmark.bench_board("board-a", 0.01)
        device.__enter__.return_value.read_info.assert_called_once()
        device.__enter__.return_value.read_configuration.assert_called_once()
        measure.assert_called_once_with(device.__enter__.return_value.read_wireless, 50)
        self.assertFalse(monitor.read_wireless.called)

    def test_invalid_sample_is_rejected(self):
        for seconds in ("0", "-1", "nan", "inf"):
            with self.subTest(seconds=seconds):
                result = subprocess.run([sys.executable, str(SCRIPT), "--seconds", seconds],
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--seconds must be positive and finite", result.stderr)
