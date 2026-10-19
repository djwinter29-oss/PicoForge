import sys
import threading
import unittest
from unittest.mock import Mock, patch

from picoforge.monitor import DeviceMonitor, _empty_snapshot


class MonitorMscTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._snapshot = {"msc": {"enabled": True, "force_return_supported": True}}

    def test_led_disconnect_after_command_preserves_disconnected_snapshot(self):
        self.monitor._snapshot = {**_empty_snapshot(), "connected": True,
                                  "board_status": {"led_available": True, "led_on": False}}

        def disconnect(on):
            with self.monitor._lock:
                self.monitor._snapshot = _empty_snapshot()

        self.monitor._active_client.set_led.side_effect = disconnect
        with self.assertRaisesRegex(RuntimeError, "disconnected"):
            self.monitor.set_led(True)
        self.monitor._active_client.set_led.assert_called_once_with(True)
        self.assertEqual(self.monitor.snapshot(), _empty_snapshot())

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


class MonitorReconnectTests(unittest.TestCase):
    def test_reconnect_snapshot_keeps_stable_api_shape(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._active_client = None
        monitor._stop = threading.Event()
        client = Mock()
        client.read_info.return_value = {"product_name": "PicoForge"}
        client.read_settings.return_value = {
            "system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 25,
        }
        client.read_input_report.side_effect = lambda timeout_ms: monitor._stop.set()
        monitor._client_factory = lambda: client
        monitor._snapshot = {}
        monitor._run()
        snapshot = monitor.snapshot()
        self.assertTrue(snapshot["connected"])
        self.assertEqual(snapshot["settings"], client.read_settings.return_value)
        self.assertTrue(snapshot["settings_supported"])
        self.assertIn("filesystem_format", snapshot)
        self.assertIsNone(snapshot["filesystem_format"])
        self.assertEqual(set(snapshot), set(_empty_snapshot()))


class MonitorSettingsTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._snapshot = {
            **_empty_snapshot(),
            "connected": True,
            "settings_supported": True,
            "settings": {"system_clock_mhz": 0, "sdio_bus_mhz": 0, "spi_bus_mhz": 0},
            "msc": {"enabled": False, "transition_pending": False},
        }

    def test_saves_settings_only_when_supported_and_storage_is_idle(self):
        settings = {"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 25}
        self.monitor._active_client.set_settings.return_value = settings
        self.assertEqual(self.monitor.set_settings(settings), settings)
        self.monitor._active_client.set_settings.assert_called_once_with(settings)
        self.assertEqual(self.monitor.snapshot()["settings"], settings)

    def test_rejects_settings_when_storage_state_is_unknown_or_busy(self):
        settings = {"system_clock_mhz": 150, "sdio_bus_mhz": 0, "spi_bus_mhz": 0}
        for msc in (None, {"enabled": True, "transition_pending": False},
                    {"enabled": False, "transition_pending": True}):
            with self.subTest(msc=msc):
                self.monitor._snapshot["msc"] = msc
                self.monitor._active_client.set_settings.reset_mock()
                with self.assertRaises(RuntimeError):
                    self.monitor.set_settings(settings)
                self.monitor._active_client.set_settings.assert_not_called()

    def test_rejects_invalid_saved_values_before_hid_write(self):
        with self.assertRaises(ValueError):
            self.monitor.set_settings({"system_clock_mhz": 100, "sdio_bus_mhz": 0, "spi_bus_mhz": 0})
        self.monitor._active_client.set_settings.assert_not_called()
