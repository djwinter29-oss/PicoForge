import sys
import threading
import unittest
from unittest.mock import Mock, patch

from picoforge.monitor import DashboardBoards, DeviceMonitor, _empty_snapshot


class MonitorMscTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._active_client.set_msc.return_value = {"status": "completed", "error": "none"}
        self.monitor._active_client.set_led.return_value = {"status": "completed", "error": "none"}
        self.monitor._snapshot = {"msc": {"enabled": True, "force_return_supported": True}}

    def test_forced_settings_require_confirmed_capable_idle_host_ownership(self):
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1}
        snapshot = {"connected": True, "settings_supported": True,
                    "hardware": {"capabilities": 68, "storage_transport": "sdio"},
                    "msc": {"enabled": True, "transition_pending": False},
                    "self_test": {"active": False}}
        self.monitor._snapshot = snapshot
        self.monitor._active_client.set_settings.return_value = settings
        self.assertEqual(self.monitor.set_settings(settings, force=True), settings)
        self.monitor._active_client.set_settings.assert_called_once_with(settings, force=True)
        for mutate in [
            {"hardware": {"capabilities": 4, "storage_transport": "sdio"}},
            {"msc": {"enabled": False, "transition_pending": False}},
            {"msc": {"enabled": True, "transition_pending": True}},
            {"self_test": {"active": True}},
        ]:
            self.monitor._snapshot = {**snapshot, **mutate}
            self.monitor._active_client.reset_mock()
            with self.assertRaises(RuntimeError):
                self.monitor.set_settings(settings, force=True)
            self.monitor._active_client.set_settings.assert_not_called()
        self.monitor._snapshot = snapshot
        with self.assertRaisesRegex(RuntimeError, "Disable"):
            self.monitor.set_settings(settings)

    def test_led_disconnect_after_command_preserves_disconnected_snapshot(self):
        self.monitor._snapshot = {**_empty_snapshot(), "connected": True,
                                  "hardware": {"led_available": True},
                                  "board_status": {"led_available": True, "led_on": False}}

        def disconnect(on):
            with self.monitor._lock:
                self.monitor._snapshot = _empty_snapshot()
            return {"status": "completed", "error": "none"}

        self.monitor._active_client.set_led.side_effect = disconnect
        status = self.monitor.set_led(True)
        self.assertTrue(status["led_on"])
        self.monitor._active_client.set_led.assert_called_once_with(True)
        snapshot = self.monitor.snapshot()
        self.assertFalse(snapshot["connected"])
        self.assertIsNone(snapshot["control_result"])

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

    def test_reboot_exposes_control_result(self):
        result = {"status": "completed", "opcode": 2, "transaction_id": 1}
        self.monitor._active_client.reboot.return_value = result
        self.monitor._snapshot = _empty_snapshot()
        self.assertEqual(self.monitor.reboot(), result)
        self.assertEqual(self.monitor.snapshot()["control_result"], result)

    def test_uncertain_command_blocks_writes_even_after_client_reconnects(self):
        self.monitor._snapshot = {
            **_empty_snapshot(),
            "connected": True,
            "hardware": {"led_available": True},
            "board_status": {"led_available": True, "led_on": False},
            "msc": {"enabled": False, "transition_pending": False},
        }
        self.monitor._active_client._command_uncertain = True
        self.monitor._active_client.set_led.side_effect = TimeoutError("uncertain")
        with self.assertRaises(TimeoutError):
            self.monitor.set_led(True)
        self.assertTrue(self.monitor.snapshot()["command_outcome_uncertain"])
        self.monitor._active_client = Mock()
        with self.assertRaisesRegex(RuntimeError, "outcome is uncertain"):
            self.monitor.set_msc(True)
        self.monitor._active_client.set_msc.assert_not_called()


class MonitorReconnectTests(unittest.TestCase):
    def test_confirmed_msc_reconnect_is_bounded_and_does_not_hide_other_errors(self):
        cases = [
            (OSError("read error"), 104.0, True, False, True),
            (RuntimeError("PicoForge HID interface not found; check USB"), 104.0, True, False, True),
            (OSError("read error"), 105.0, True, False, False),
            (OSError("read error"), 104.0, False, False, False),
            (OSError("read error"), 104.0, True, True, False),
            (ValueError("invalid report"), 104.0, True, False, False),
            (RuntimeError("unexpected failure"), 104.0, True, False, False),
        ]
        for error, now, confirmed, uncertain, reconnecting in cases:
            with self.subTest(error=error, now=now, confirmed=confirmed, uncertain=uncertain):
                monitor = DeviceMonitor.__new__(DeviceMonitor)
                monitor._device_lock = threading.RLock()
                monitor._lock = threading.RLock()
                monitor._stop = threading.Event()
                monitor._stop.wait = lambda timeout: monitor._stop.set()
                monitor._active_client = Mock()
                monitor._active_client.set_msc.return_value = {"status": "completed", "error": "none"}
                monitor._snapshot = {**_empty_snapshot(), "msc": {"enabled": False}}
                if confirmed:
                    with patch("picoforge.monitor.time.monotonic", return_value=100.0):
                        monitor.set_msc(True)
                monitor._command_outcome_uncertain = uncertain
                monitor._client_factory = Mock(side_effect=error)
                with patch("picoforge.monitor.time.monotonic", return_value=now):
                    monitor._run()
                snapshot = monitor.snapshot()
                self.assertFalse(snapshot["connected"])
                self.assertEqual(snapshot["reconnecting"], reconnecting)
                self.assertEqual(snapshot["error"], None if reconnecting else str(error))

    def test_reconnect_snapshot_keeps_stable_api_shape(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._active_client = None
        monitor._stop = threading.Event()
        client = Mock()
        client.read_info.return_value = {
            "product_name": "PicoForge", "settings_supported": True,
            "storage_transport": "sdio",
            "self_test_supported": True,
        }
        client.read_configuration.return_value = {
            "system_clock_mhz": 200, "sdcard_profile": 1, "stored_sysclk_profile": 200,
            "stored_sdcard_profile": 1, "storage_transport": "sdio",
            "sdio_bus_mhz": 50, "spi_bus_mhz": 0,
        }
        client.read_input_report.side_effect = lambda timeout_ms: monitor._stop.set()
        monitor._client_factory = lambda: client
        monitor._snapshot = {}
        monitor._run()
        snapshot = monitor.snapshot()
        self.assertTrue(snapshot["connected"])
        self.assertEqual(snapshot["settings"], {"system_clock_mhz": 200, "sdcard_profile": 1})
        self.assertEqual(snapshot["board_configuration"], client.read_configuration.return_value)
        self.assertTrue(snapshot["settings_supported"])
        self.assertIn("filesystem_format", snapshot)
        self.assertIsNone(snapshot["filesystem_format"])
        self.assertEqual(set(snapshot), set(_empty_snapshot()))

    def test_storage_and_self_test_reports_publish_state_and_freshness(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._active_client = None
        monitor._stop = threading.Event()
        storage = {
            "filesystem_format": "fat32",
            "msc": {"enabled": False, "transition_pending": False, "request_rejected": False},
        }
        self_test = {"state": "idle", "active": False}
        client = Mock()
        client.read_info.return_value = {
            "product_name": "PicoForge", "settings_supported": False,
            "storage_transport": "sdio", "self_test_supported": True,
        }
        client.read_configuration.return_value = {
            "system_clock_mhz": 0,
            "sdcard_profile": 0,
        }
        captured = {}
        items = iter([
            {"kind": "memory", "data": storage},
            {"kind": "self_test", "data": self_test},
            None,
        ])

        def read_input_report(timeout_ms):
            item = next(items)
            if item is None:
                captured["snapshot"] = monitor.snapshot()
                monitor._stop.set()
            return item

        client.read_input_report.side_effect = read_input_report
        monitor._client_factory = lambda: client
        monitor._snapshot = _empty_snapshot()
        monitor._input_received_at = {}
        monitor._command_outcome_uncertain = False
        monitor._run()

        snapshot = captured["snapshot"]
        self.assertEqual(snapshot["memory"], storage)
        self.assertEqual(snapshot["filesystem_format"], "fat32")
        self.assertEqual(snapshot["msc"], storage["msc"])
        self.assertEqual(snapshot["self_test"], self_test)
        self.assertTrue(snapshot["report_freshness"]["10"]["available"])
        self.assertTrue(snapshot["report_freshness"]["11"]["available"])

    def test_input_freshness_is_tracked_and_expires_per_report(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._lock = threading.RLock()
        monitor._snapshot = {
            **_empty_snapshot(),
            "hardware": {"self_test_supported": True},
            "runtime": {"uptime_ms": 1},
            "memory": {"sample_valid": True},
            "self_test": {"active": True},
        }
        monitor._input_received_at = {3: 99.5, 10: 98.0, 11: 98.0}
        with patch("picoforge.monitor.time.monotonic", return_value=100.0):
            snapshot = monitor.snapshot()
        freshness = snapshot["report_freshness"]
        self.assertFalse(freshness["3"]["stale"])
        self.assertTrue(freshness["10"]["stale"])
        self.assertTrue(freshness["11"]["stale"])
        self.assertEqual(freshness["3"]["age_ms"], 500)
        self.assertIsNone(snapshot["memory"])
        self.assertIsNone(snapshot["self_test"])
        self.assertEqual(snapshot["runtime"], {"uptime_ms": 1})


class MonitorSettingsTests(unittest.TestCase):
    def setUp(self):
        self.monitor = DeviceMonitor.__new__(DeviceMonitor)
        self.monitor._device_lock = threading.RLock()
        self.monitor._lock = threading.RLock()
        self.monitor._active_client = Mock()
        self.monitor._snapshot = {
            **_empty_snapshot(),
            "connected": True,
            "hardware": {"storage_transport": "sdio"},
            "settings_supported": True,
            "settings": {"system_clock_mhz": 0, "sdcard_profile": 0,
                         "sdio_bus_mhz": 25, "spi_bus_mhz": 0},
            "msc": {"enabled": False, "transition_pending": False},
        }

    def test_saves_settings_only_when_supported_and_storage_is_idle(self):
        settings = {"system_clock_mhz": 200, "sdio_bus_mhz": 50, "spi_bus_mhz": 50}
        self.monitor._active_client.set_settings.return_value = {
            "system_clock_mhz": 200, "sdcard_profile": 1,
        }
        self.monitor._active_client.configuration = {
            "system_clock_mhz": 200, "sdcard_profile": 1,
            "sdio_bus_mhz": 50, "spi_bus_mhz": 0,
        }
        saved = self.monitor.set_settings(settings)
        self.assertEqual(saved["sdcard_profile"], 1)
        self.monitor._active_client.set_settings.assert_called_once_with(
            {"system_clock_mhz": 200, "sdcard_profile": 1}
        )
        self.assertIn("Waiting for the board to reboot", self.monitor.snapshot()["settings_message"])

    def test_canonical_settings_are_saved_and_returned_without_transport_fields(self):
        settings = {"system_clock_mhz": 150, "sdcard_profile": 0}
        self.monitor._active_client.set_settings.return_value = settings
        self.monitor._active_client.configuration = {"system_clock_mhz": 150, "sdcard_profile": 0}
        self.assertEqual(self.monitor.set_settings(settings), settings)
        self.monitor._active_client.set_settings.assert_called_once_with(settings)

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


class DashboardBoardsTests(unittest.TestCase):
    def test_boards_are_lazily_monitored_and_reused_by_stable_serial_id(self):
        attached = [
            {"product_name": "PicoForge", "serial_number": "board-A", "device_path": "/hid/a"},
            {"product_name": "PicoForge", "serial_number": "board-B", "device_path": "/hid/b"},
        ]
        created = []

        class FakeMonitor:
            def __init__(self, serial, path):
                self.serial = serial
                self.path = path
                self.closed = False

            def snapshot(self):
                return {"serial": self.serial, "device_path": self.path}

            def set_led(self, on):
                return {"led_on": on}

            def close(self):
                self.closed = True

        def create_monitor(serial, path):
            monitor = FakeMonitor(serial, path)
            created.append(monitor)
            return monitor

        boards = DashboardBoards(discover=lambda: attached, monitor_factory=create_monitor)
        listing = boards.boards()
        first, second = listing["boards"]
        self.assertEqual(first["id"], listing["default_board_id"])
        self.assertEqual(created, [])
        self.assertEqual(boards.snapshot(first["id"])["serial"], "board-A")
        self.assertEqual(boards.set_led(second["id"], True), {"led_on": True})
        self.assertEqual(len(created), 2)
        attached[0]["device_path"] = "/hid/new-a"
        rescanned = boards.boards()
        self.assertEqual(rescanned["boards"][0]["id"], first["id"])
        self.assertEqual(boards.snapshot(first["id"])["device_path"], None)
        boards.close()
        self.assertTrue(all(monitor.closed for monitor in created))

    def test_unknown_board_and_duplicate_identity_are_rejected(self):
        attached = [{"serial_number": "duplicate", "device_path": "/hid/a"}]
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: Mock())
        with self.assertRaisesRegex(RuntimeError, "unknown board"):
            boards.snapshot("unknown")
        boards.boards()
        attached.append({"serial_number": "duplicate", "device_path": "/hid/b"})
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            boards.boards()
        boards.close()
