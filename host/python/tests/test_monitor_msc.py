import sys
import threading
import unittest
from unittest.mock import Mock, patch

from picoforge.monitor import DeviceMonitor


class MonitorMscTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._snapshot = {"msc": {"enabled": True, "force_return_supported": True}}

    def test_confirmed_return_uses_only_hid_on_each_host_platform(self):
        for platform in ("linux", "win32", "darwin"):
            with self.subTest(platform=platform), patch.object(sys, "platform", platform), \
                    patch("subprocess.run", side_effect=AssertionError("Host eject must not run")):
                self.monitor._snapshot["msc"]["transition_pending"] = False
                self.monitor._active_client.reset_mock()
                status = self.monitor.set_msc(False, force=True)
                self.monitor._active_client.set_msc.assert_called_once_with(False, force=True)
                self.assertTrue(status["transition_pending"])

    def test_legacy_safe_command_remains_hid_only(self):
        self.monitor.set_msc(False)
        self.monitor._active_client.set_msc.assert_called_once_with(False)

    def test_return_requires_capable_enabled_idle_firmware(self):
        for field, value in (("force_return_supported", False), ("enabled", False),
                             ("transition_pending", True)):
            with self.subTest(field=field):
                self.monitor._snapshot["msc"] = {
                    "enabled": True, "force_return_supported": True, "transition_pending": False, field: value,
                }
                with self.assertRaisesRegex(RuntimeError, "force-return support"):
                    self.monitor.set_msc(False, force=True)
                self.monitor._active_client.set_msc.assert_not_called()

    def test_enable_keeps_existing_command(self):
        self.monitor.set_msc(True)
        self.monitor._active_client.set_msc.assert_called_once_with(True)
