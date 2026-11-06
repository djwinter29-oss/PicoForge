"""Board discovery, monitor reuse and monitor retirement for the Python dashboard."""

import threading
import time
import unittest
from unittest.mock import Mock, patch

from helpers import make_monitor
from picoforge.dashboard.boards import DashboardBoards
from picoforge.dashboard.monitor import BoardConflict, CommandOutcomeUncertain


class IdleMonitor:
    def try_retire(self):
        return True


class DashboardBoardsTests(unittest.TestCase):
    def test_expiration_overlapping_a_command_preserves_the_result_and_write_block(self):
        for uncertain in (False, True):
            with self.subTest(uncertain=uncertain):
                attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
                now = [0.0]
                entered = threading.Event()
                release = threading.Event()
                errors = []
                client = Mock()
                monitor = make_monitor(client, {"connected": True, "hardware": {"led_available": True}})
                factory = Mock(return_value=monitor)
                boards = DashboardBoards(discover=lambda: attached, monitor_factory=factory, clock=lambda: now[0])
                board_id = boards.boards()["boards"][0]["id"]
                self.assertIs(boards.monitor(board_id), monitor)

                def send(_on):
                    entered.set()
                    if not release.wait(2):
                        raise AssertionError("command release timed out")
                    if uncertain:
                        client.command_uncertain = True
                        client.uncertain_command = (1, 1)
                        client.uncertain_wireless_operation = None
                        raise TimeoutError("outcome not confirmed")
                    return {"status": "completed", "error": "none"}

                def command():
                    try:
                        monitor.set_led(True)
                    except Exception as error:
                        errors.append(error)

                client.set_led.side_effect = send
                worker = threading.Thread(target=command)
                worker.start()
                try:
                    self.assertTrue(entered.wait(1))
                    attached.clear()
                    now[0] = 31.0
                    boards.boards()
                    self.assertFalse(monitor._stop.is_set(), "active command must prevent retirement")
                    self.assertFalse(boards._retiring)
                    self.assertIs(boards.monitor(board_id), monitor)
                    release.set()
                    worker.join(2)
                    self.assertFalse(worker.is_alive())
                    boards.boards()
                    if uncertain:
                        self.assertEqual(len(errors), 1)
                        self.assertIsInstance(errors[0], CommandOutcomeUncertain)
                        attached.append({"serial_number": "board-A", "device_path": "/hid/new"})
                        boards.boards()
                        self.assertIs(boards.monitor(board_id), monitor)
                        self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
                        with self.assertRaisesRegex(RuntimeError, "uncertain"):
                            monitor.set_led(False)
                        client.set_led.assert_called_once()
                        factory.assert_called_once()
                    else:
                        self.assertEqual(errors, [])
                        self.assertTrue(monitor._stop.is_set())
                        with self.assertRaisesRegex(LookupError, "unknown board"):
                            boards.monitor(board_id)
                finally:
                    release.set()
                    worker.join(2)
                    boards.close()

    def test_retirement_claim_prevents_already_obtained_monitors_from_starting_commands(self):
        client = Mock()
        monitor = make_monitor(client, {"connected": True, "hardware": {"led_available": True}})
        self.assertTrue(monitor.try_retire())
        with self.assertRaises(BoardConflict):
            monitor.set_led(True)
        with self.assertRaises(BoardConflict):
            monitor.resync_command()
        client.set_led.assert_not_called()
        client.reconcile_outcome.assert_not_called()
        monitor.close()

    def test_boards_are_lazily_monitored_and_reused_by_stable_serial_id(self):
        attached = [
            {"product_name": "PicoForge", "serial_number": "board-A", "device_path": "/hid/a"},
            {"product_name": "PicoForge", "serial_number": "board-B", "device_path": "/hid/b"},
        ]
        created = []

        class FakeMonitor(IdleMonitor):
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
        self.assertEqual(boards.monitor(first["id"]).snapshot()["serial"], "board-A")
        self.assertEqual(boards.monitor(second["id"]).set_led(True), {"led_on": True})
        self.assertEqual(len(created), 2)
        attached[0]["device_path"] = "/hid/new-a"
        rescanned = boards.boards()
        self.assertEqual(rescanned["boards"][0]["id"], first["id"])
        self.assertEqual(boards.monitor(first["id"]).snapshot()["device_path"], None)
        boards.close()
        self.assertTrue(all(monitor.closed for monitor in created))

    def test_unknown_board_is_rejected_and_duplicate_identity_is_quarantined(self):
        attached = [{"serial_number": "duplicate", "device_path": "/hid/a"}]
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: Mock())
        with self.assertRaisesRegex(LookupError, "unknown board"):
            boards.monitor("unknown").snapshot()
        boards.boards()
        attached.append({"serial_number": "duplicate", "device_path": "/hid/b"})
        listing = boards.boards()
        self.assertEqual(listing["boards"], [])
        self.assertIn("Duplicate", listing["warnings"][0])
        with self.assertRaisesRegex(BoardConflict, "Duplicate"):
            boards.monitor(list(boards._ambiguous)[0])
        boards.close()

    def test_removed_serial_board_monitor_expires_after_reconnect_grace_period(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        created = []
        closed = threading.Event()

        class FakeMonitor(IdleMonitor):
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
        boards.monitor(board_id).snapshot()
        attached.clear()

        now[0] = 29.0
        boards.boards()
        self.assertFalse(created[0].closed)
        self.assertEqual(boards.monitor(board_id).snapshot(), {"command_outcome_uncertain": False})

        now[0] = 30.0
        boards.boards()
        self.assertTrue(closed.wait(1))
        self.assertTrue(created[0].closed)
        with self.assertRaisesRegex(LookupError, "unknown board"):
            boards.monitor(board_id).snapshot()

        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        attached.append({"serial_number": "board-A", "device_path": "/hid/new-a"})
        self.assertEqual(boards.boards()["boards"][0]["id"], board_id)
        boards.monitor(board_id).snapshot()
        self.assertEqual(len(created), 2)
        boards.close()

    def test_removed_path_only_board_expires_after_short_grace_period(self):
        attached = [{"serial_number": None, "device_path": "/hid/a"}]
        now = [0.0]
        created = []
        closed = threading.Event()

        class FakeMonitor(IdleMonitor):
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
        boards.monitor(board_id).snapshot()
        attached.clear()
        now[0] = 5.0
        boards.boards()
        self.assertTrue(closed.wait(1))
        self.assertTrue(created[0].closed)
        with self.assertRaisesRegex(LookupError, "unknown board"):
            boards.monitor(board_id).snapshot()
        boards.close()

    def test_failed_discovery_does_not_expire_known_monitors(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": False}
        discover = Mock(return_value=attached)
        boards = DashboardBoards(discover=discover, monitor_factory=lambda *_: monitor, clock=lambda: now[0])
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id).snapshot()

        now[0] = 31.0
        discover.side_effect = OSError("HID enumeration failed")
        with self.assertRaisesRegex(OSError, "enumeration failed"):
            boards.boards()
        self.assertFalse(monitor.close.called)
        self.assertEqual(boards.monitor(board_id).snapshot(), monitor.snapshot.return_value)
        boards.close()

    def test_uncertain_command_keeps_stale_monitor_to_preserve_write_block(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": True}
        monitor.try_retire.return_value = False
        boards = DashboardBoards(
            discover=lambda: attached, monitor_factory=lambda *_: monitor, clock=lambda: now[0]
        )
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id).snapshot()
        attached.clear()
        now[0] = 60.0
        boards.boards()

        monitor.close.assert_not_called()
        self.assertEqual(boards.monitor(board_id).snapshot(), monitor.snapshot.return_value)
        boards.close()
        monitor.close.assert_called_once()

    def test_monitor_close_waits_until_background_thread_stops(self):
        monitor = make_monitor(fresh=False)
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

        class FakeMonitor(IdleMonitor):
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
        boards.monitor(board_id).snapshot()
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
            boards.monitor(board_id).snapshot()
        self.assertEqual(len(created), 1)

        release_close.set()
        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        boards.monitor(board_id).snapshot()
        self.assertEqual(len(created), 2)
        boards.close()

    def test_failed_retirement_keeps_the_board_refused_until_restart(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        close_attempted = threading.Event()
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": False}

        def fail_close():
            close_attempted.set()
            raise ValueError("unexpected cleanup failure")

        monitor.close.side_effect = fail_close
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: monitor,
                                 clock=lambda: now[0])
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id)
        attached.clear()
        now[0] = 30.0
        with patch("picoforge.dashboard.boards._LOGGER.exception") as logged:
            boards.boards()
            self.assertTrue(close_attempted.wait(1))
            deadline = time.monotonic() + 1
            while not logged.called and time.monotonic() < deadline:
                time.sleep(0.001)
        self.assertTrue(logged.called)
        attached.append({"serial_number": "board-A", "device_path": "/hid/new-a"})
        boards.boards()
        with self.assertRaisesRegex(RuntimeError, "shutting down"):
            boards.monitor(board_id)
        monitor.close.side_effect = None
        with self.assertRaisesRegex(RuntimeError, "Failed to close 1"):
            boards.close()
        monitor.close.assert_called_once()

    def test_shutdown_joins_retirement_without_closing_the_monitor_twice(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        now = [0.0]
        closing = threading.Event()
        release = threading.Event()
        finished = threading.Event()
        monitor = Mock()
        monitor.snapshot.return_value = {"command_outcome_uncertain": False}

        def close():
            closing.set()
            release.wait(2)

        monitor.close.side_effect = close
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: monitor, clock=lambda: now[0])
        boards.monitor(boards.boards()["boards"][0]["id"])
        attached.clear()
        now[0] = 30.0
        boards.boards()
        self.assertTrue(closing.wait(1))
        shutdown = threading.Thread(target=lambda: (boards.close(), finished.set()))
        shutdown.start()
        try:
            self.assertFalse(finished.wait(0.05))
            monitor.close.assert_called_once()
        finally:
            release.set()
            shutdown.join(2)
        self.assertTrue(finished.is_set())
        boards.close()
        monitor.close.assert_called_once()

    def test_discovery_does_not_hold_registry_lock_and_cannot_publish_after_shutdown(self):
        discovering = threading.Event()
        release = threading.Event()
        errors = []
        board = {"serial_number": "board-A", "device_path": "/hid/a"}
        discover = Mock(return_value=[board])
        monitor = Mock()
        boards = DashboardBoards(discover=discover, monitor_factory=lambda *_: monitor)
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id)

        def slow_discover():
            discovering.set()
            release.wait(2)
            return [board]

        discover.side_effect = slow_discover

        def rescan():
            try:
                boards.boards()
            except RuntimeError as error:
                errors.append(str(error))

        worker = threading.Thread(target=rescan)
        worker.start()
        self.assertTrue(discovering.wait(1))
        try:
            acquired = boards._lock.acquire(timeout=0.2)
            self.assertTrue(acquired)
            if acquired:
                boards._lock.release()
            self.assertIs(boards.monitor(board_id), monitor)
            boards.close()
        finally:
            release.set()
            worker.join(2)
        self.assertEqual(errors, ["dashboard is shutting down"])
        monitor.close.assert_called_once()

    def test_path_identity_preserves_non_utf8_bytes(self):
        boards = DashboardBoards(discover=lambda: [{"device_path": "/hid/\udcff", "serial_number": None}])
        first = boards.boards()["boards"][0]
        self.assertEqual(first, boards.boards()["boards"][0])
        self.assertEqual(first["device_path"].encode("utf-8", "surrogateescape"), b"/hid/\xff")
        boards.close()

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
        count_lock = threading.Lock()
        created = []

        class FakeMonitor(IdleMonitor):
            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                first_close_started.set()
                release_close.wait()
                with count_lock:
                    close_count[0] += 1
                    if close_count[0] == 16:
                        all_closed.set()

        boards = DashboardBoards(
            discover=lambda: attached,
            monitor_factory=lambda *_: created.append(FakeMonitor()) or created[-1],
            clock=lambda: now[0],
        )
        for board in boards.boards()["boards"]:
            boards.monitor(board["id"]).snapshot()
        attached.clear()
        now[0] = 30.0
        boards.boards()
        self.assertTrue(first_close_started.wait(1))
        self.assertEqual(len(boards._retiring), 16)
        attached.append({"serial_number": "board-16", "device_path": "/hid/16"})
        new_board_id = boards.boards()["boards"][0]["id"]
        with self.assertRaisesRegex(RuntimeError, "capacity reached"):
            boards.monitor(new_board_id).snapshot()
        self.assertEqual(len(created), 16)

        release_close.set()
        self.assertTrue(all_closed.wait(1))
        deadline = time.monotonic() + 1
        while boards._retiring and time.monotonic() < deadline:
            time.sleep(0.001)
        self.assertFalse(boards._retiring)
        boards.monitor(new_board_id).snapshot()
        self.assertEqual(len(created), 17)
        boards.close()

    def test_shutdown_waits_for_monitor_cleanup_to_finish(self):
        attached = [{"serial_number": "board-A", "device_path": "/hid/a"}]
        close_started = threading.Event()
        release_close = threading.Event()
        close_finished = threading.Event()

        class FakeMonitor(IdleMonitor):
            def snapshot(self):
                return {"command_outcome_uncertain": False}

            def close(self):
                close_started.set()
                release_close.wait()

        boards = DashboardBoards(discover=lambda: attached, monitor_factory=lambda *_: FakeMonitor())
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id).snapshot()

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

        class FakeMonitor(IdleMonitor):
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
            boards.monitor(board["id"]).snapshot()

        with patch("picoforge.dashboard.boards._LOGGER.exception"):
            with self.assertRaisesRegex(RuntimeError, "Failed to close 1 dashboard monitor"):
                boards.close()
        self.assertCountEqual(closed, ["board-A", "board-B"])


if __name__ == "__main__":
    unittest.main()
