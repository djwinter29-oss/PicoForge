import sys
import threading
import time
import unittest
from unittest.mock import Mock, patch

from picoforge.dashboard.boards import DashboardBoards
from picoforge.dashboard.monitor import DeviceMonitor, _empty_snapshot


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
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0}
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
        self.assertEqual(self.monitor._msc_reconnect_deadline, 0.0)
        self.assertIsNone(self.monitor._msc_reconnect_enabled)

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

    def test_snapshot_mutations_do_not_change_monitor_state(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._lock = threading.RLock()
        monitor._snapshot = {
            **_empty_snapshot(),
            "hardware": {"product_name": "PicoForge"},
            "runtime": {"uptime_ms": 42},
        }
        monitor._input_received_at = {}
        monitor._command_outcome_uncertain = False

        snapshot = monitor.snapshot()
        snapshot["hardware"]["product_name"] = "changed"
        snapshot["runtime"]["uptime_ms"] = 0

        self.assertEqual(monitor._snapshot["hardware"]["product_name"], "PicoForge")
        self.assertEqual(monitor._snapshot["runtime"]["uptime_ms"], 42)


class MonitorReconnectTests(unittest.TestCase):
    def test_monitor_yields_outside_the_hid_lock_between_nonblocking_input_batches(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._device_lock = threading.RLock()
        monitor._lock = threading.RLock()
        monitor._stop = threading.Event()
        monitor._active_client = None
        monitor._snapshot = _empty_snapshot()
        monitor._input_received_at = {}
        client = Mock()
        client.read_info.return_value = {"supported": True}
        client.read_configuration.return_value = {"system_clock_mhz": 0, "sdcard_profile": 0}
        client.read_input_report.side_effect = lambda timeout_ms: monitor._stop.set()
        monitor._client_factory = lambda: client
        waits = []

        def wait(timeout):
            waits.append(timeout)
            if timeout != 0.05:
                return
            acquired = []

            def acquire():
                locked = monitor._device_lock.acquire(blocking=False)
                acquired.append(locked)
                if locked:
                    monitor._device_lock.release()

            worker = threading.Thread(target=acquire)
            worker.start()
            worker.join(timeout=1)
            self.assertEqual(acquired, [True], "Commands must be able to acquire HID between input batches")

        monitor._stop.wait = wait
        monitor._run()
        client.read_input_report.assert_called_once_with(timeout_ms=0)
        self.assertIn(0.05, waits)

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
                    with patch("picoforge.dashboard.monitor.time.monotonic", return_value=100.0):
                        monitor.set_msc(True)
                monitor._command_outcome_uncertain = uncertain
                monitor._client_factory = Mock(side_effect=error)
                with patch("picoforge.dashboard.monitor.time.monotonic", return_value=now):
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
        self.assertEqual(snapshot["settings"], {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0})
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
        with patch("picoforge.dashboard.monitor.time.monotonic", return_value=100.0):
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
            {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0}
        )
        self.assertIn("Waiting for the board to reboot", self.monitor.snapshot()["settings_message"])

    def test_canonical_settings_are_saved_and_returned_without_transport_fields(self):
        settings = {"system_clock_mhz": 150, "sdcard_profile": 0, "radios": 0}
        self.monitor._active_client.set_settings.return_value = settings
        self.monitor._active_client.configuration = {"system_clock_mhz": 150, "sdcard_profile": 0}
        self.assertEqual(self.monitor.set_settings(settings), settings)
        self.monitor._active_client.set_settings.assert_called_once_with(settings)

    def test_settings_without_radios_keep_the_saved_radio_enables(self):
        self.monitor._snapshot["board_configuration"] = {"radios": 3}
        self.monitor.set_settings({"system_clock_mhz": 150, "sdcard_profile": 0})
        self.monitor._active_client.set_settings.assert_called_once_with(
            {"system_clock_mhz": 150, "sdcard_profile": 0, "radios": 3})

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

    def test_removed_serial_board_monitor_expires_after_reconnect_grace_period(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        created = []
        closed = threading.Event()

        class FakeMonitor:
            closed = False

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                self.closed = True
                closed.set()

        def create_monitor(*_):
            monitor = FakeMonitor()
            created.append(monitor)
            return monitor

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=create_monitor,
            clock=lambda: now[0],
        )
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)
        attached.clear()

        now[0] = 29.0
        boards.boards()
        self.assertFalse(created[0].closed)
        self.assertEqual(boards.snapshot(board_id), {"command_outcome_uncertain": False})

        now[0] = 30.0
        boards.boards()
        self.assertTrue(closed.wait(1))
        self.assertTrue(created[0].closed)
        with self.assertRaisesRegex(RuntimeError, "unknown board"):
            boards.snapshot(board_id)

        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        attached.append({"serial_number": "board-A", "device_path": "/hid/new-a"})
        self.assertEqual(boards.boards()["boards"][0]["id"], board_id)
        boards.snapshot(board_id)
        self.assertEqual(len(created), 2)
        boards.close()

    def test_removed_path_only_board_expires_after_short_grace_period(self):
        attached = [{"serial_number": None, "device_path": "/hid/a"}]
        now = [0.0]
        created = []
        closed = threading.Event()

        class FakeMonitor:
            closed = False

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                self.closed = True
                closed.set()

        def create_monitor(*_):
            monitor = FakeMonitor()
            created.append(monitor)
            return monitor

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=create_monitor,
            clock=lambda: now[0],
        )
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)
        attached.clear()
        now[0] = 5.0
        boards.boards()
        self.assertTrue(closed.wait(1))
        self.assertTrue(created[0].closed)
        with self.assertRaisesRegex(RuntimeError, "unknown board"):
            boards.snapshot(board_id)
        boards.close()

    def test_failed_discovery_does_not_expire_known_monitors(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": False}
        discover = Mock(return_value=attached)
        boards = DashboardBoards(discover=discover, monitor_factory=lambda *_: monitor, clock=lambda: now[0])
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)

        now[0] = 31.0
        discover.side_effect = OSError("HID enumeration failed")
        with self.assertRaisesRegex(OSError, "enumeration failed"):
            boards.boards()
        self.assertFalse(monitor.close.called)
        self.assertEqual(boards.snapshot(board_id), monitor.snapshot.return_value)
        boards.close()

    def test_uncertain_command_keeps_stale_monitor_to_preserve_write_block(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": True}
        boards = DashboardBoards(
            discover=lambda: attached, monitor_factory=lambda *_: monitor, clock=lambda: now[0]
        )
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)
        attached.clear()
        now[0] = 60.0
        boards.boards()

        monitor.close.assert_not_called()
        self.assertEqual(boards.snapshot(board_id), monitor.snapshot.return_value)
        boards.close()
        monitor.close.assert_called_once()

    def test_monitor_close_waits_until_background_thread_stops(self):
        monitor = DeviceMonitor.__new__(DeviceMonitor)
        monitor._stop = threading.Event()
        started = threading.Event()
        release = threading.Event()
        closed = threading.Event()

        def run():
            started.set()
            release.wait()

        monitor._thread = threading.Thread(target=run)
        monitor._thread.start()
        self.assertTrue(started.wait(1))

        def close():
            monitor.close()
            closed.set()

        closer = threading.Thread(target=close)
        closer.start()
        self.assertFalse(closed.wait(0.05))
        release.set()
        self.assertTrue(closed.wait(1))
        closer.join()

    def test_reconnected_board_cannot_start_a_second_monitor_while_old_one_closes(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        closing = threading.Event()
        release_close = threading.Event()
        created = []

        class FakeMonitor:
            def __init__(self, wait_on_close=False):
                self.wait_on_close = wait_on_close

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                if self.wait_on_close:
                    closing.set()
                    release_close.wait()

        def create_monitor(*_):
            monitor = FakeMonitor(wait_on_close=not created)
            created.append(monitor)
            return monitor

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=create_monitor,
            clock=lambda: now[0],
        )
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)
        attached.clear()
        now[0] = 30.0
        expiry = threading.Thread(target=boards.boards)
        expiry.start()
        self.assertTrue(closing.wait(1))
        expiry.join(1)
        self.assertFalse(expiry.is_alive())

        attached.append({"serial_number": "board-A", "device_path": "/hid/new-a"})
        boards.boards()
        with self.assertRaisesRegex(RuntimeError, "shutting down"):
            boards.snapshot(board_id)
        self.assertEqual(len(created), 1)

        release_close.set()
        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        boards.snapshot(board_id)
        self.assertEqual(len(created), 2)
        boards.close()

    def test_expired_monitors_share_one_retirement_worker(self):
        attached = [
            {"serial_number": "board-A", "device_path": "/hid/a"},
            {"serial_number": "board-B", "device_path": "/hid/b"},
        ]
        now = [0.0]
        closed = []
        all_closed = threading.Event()

        class FakeMonitor:
            def __init__(self):
                self.retirement_thread = None

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                self.retirement_thread = threading.current_thread()
                closed.append(self)
                if len(closed) == 2:
                    all_closed.set()

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=lambda *_: FakeMonitor(),
            clock=lambda: now[0],
        )
        self.assertIsNone(boards._retirement_thread)
        for board in boards.boards()["boards"]:
            boards.snapshot(board["id"])
        attached.clear()
        now[0] = 30.0
        boards.boards()

        self.assertIsNotNone(boards._retirement_thread)
        self.assertTrue(all_closed.wait(1))
        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        self.assertEqual(len({monitor.retirement_thread for monitor in closed}), 1)
        self.assertEqual(closed[0].retirement_thread.name, "picoforge-monitor-retirement")
        self.assertFalse(boards._retiring)
        boards.close()

    def test_retirement_retries_failures_and_worker_continues(self):
        attached = [
            {"serial_number": "board-A", "device_path": "/hid/a"},
            {"serial_number": "board-B", "device_path": "/hid/b"},
        ]
        now = [0.0]
        monitors = {}
        second_closed = threading.Event()

        class FakeMonitor:
            def __init__(self, serial):
                self.serial = serial
                self.close_count = 0

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                self.close_count += 1
                if self.serial == "board-A" and self.close_count <= 3:
                    raise ValueError("unexpected cleanup failure")
                if self.serial == "board-B":
                    second_closed.set()

        def create_monitor(serial, _path):
            monitors[serial] = FakeMonitor(serial)
            return monitors[serial]

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=create_monitor,
            clock=lambda: now[0],
        )
        board_ids = {}
        for board in boards.boards()["boards"]:
            boards.snapshot(board["id"])
            board_ids[board["serial_number"]] = board["id"]
        attached.clear()
        now[0] = 30.0
        with patch("picoforge.dashboard.boards._LOGGER.exception"):
            boards.boards()
            self.assertTrue(second_closed.wait(1))
            deadline = time.monotonic() + 1
            board_a_id = board_ids["board-A"]
            while board_a_id not in boards._retirement_failures and time.monotonic() < deadline:
                time.sleep(0.001)
        self.assertEqual(monitors["board-A"].close_count, 3)
        self.assertIn(board_a_id, boards._retirement_failures)
        self.assertNotIn(board_ids["board-B"], boards._retiring)

        attached.append({"serial_number": "board-A", "device_path": "/hid/new-a"})
        boards.boards()
        with self.assertRaisesRegex(RuntimeError, "shutdown failed"):
            boards.snapshot(board_a_id)
        boards.close()
        self.assertEqual(monitors["board-A"].close_count, 4)

    def test_managed_monitor_limit_bounds_retirement_backlog(self):
        attached = [
            {"serial_number": f"board-{index}", "device_path": f"/hid/{index}"}
            for index in range(16)
        ]
        now = [0.0]
        first_close_started = threading.Event()
        release_close = threading.Event()
        all_closed = threading.Event()
        close_count = [0]
        created = []

        class FakeMonitor:
            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                close_count[0] += 1
                if close_count[0] == 1:
                    first_close_started.set()
                    release_close.wait()
                if close_count[0] == 16:
                    all_closed.set()

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=lambda *_: created.append(FakeMonitor()) or created[-1],
            clock=lambda: now[0],
        )
        for board in boards.boards()["boards"]:
            boards.snapshot(board["id"])
        attached.clear()
        now[0] = 30.0
        boards.boards()
        self.assertTrue(first_close_started.wait(1))
        self.assertEqual(len(boards._retiring), 16)
        self.assertEqual(boards._retirement_queue.maxsize, 16)
        attached.append({"serial_number": "board-16", "device_path": "/hid/16"})
        new_board_id = boards.boards()["boards"][0]["id"]
        with self.assertRaisesRegex(RuntimeError, "capacity reached"):
            boards.snapshot(new_board_id)
        self.assertEqual(len(created), 16)

        release_close.set()
        self.assertTrue(all_closed.wait(1))
        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        boards.snapshot(new_board_id)
        self.assertEqual(len(created), 17)
        boards.close()

    def test_shutdown_waits_for_monitor_cleanup_to_finish(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        close_started = threading.Event()
        release_close = threading.Event()
        close_finished = threading.Event()

        class FakeMonitor:
            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                close_started.set()
                release_close.wait()

        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: FakeMonitor())
        board_id = boards.boards()["boards"][0]["id"]
        boards.snapshot(board_id)

        closer = threading.Thread(target=lambda: (boards.close(), close_finished.set()))
        closer.start()
        self.assertTrue(close_started.wait(1))
        self.assertFalse(close_finished.is_set())
        release_close.set()
        closer.join(1)
        self.assertTrue(close_finished.is_set())

    def test_shutdown_attempts_all_monitors_after_a_close_failure(self):
        attached = [
            {"serial_number": serial, "device_path": f"/hid/{serial}"}
            for serial in ("board-A", "board-B")
        ]
        closed: list[str | None] = []

        class FakeMonitor:
            def __init__(self, serial: str | None):
                self.serial = serial

            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                closed.append(self.serial)
                if self.serial == "board-A":
                    raise OSError("close failed")

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=lambda serial, _path: FakeMonitor(serial),
        )
        for board in boards.boards()["boards"]:
            boards.snapshot(board["id"])

        with patch("picoforge.dashboard.boards._LOGGER.exception"):
            with self.assertRaisesRegex(RuntimeError, "Failed to close 1 dashboard monitor"):
                boards.close()
        self.assertCountEqual(closed, ["board-A", "board-B"])
