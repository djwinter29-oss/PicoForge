"""Cross-operation invariants through the production monitor, HID client and HTTP routes."""

import threading
import unittest
from contextlib import contextmanager
from unittest.mock import Mock, patch

from helpers import make_monitor
import test_device
import test_wireless
from test_protocol import configuration_report, identity_report, storage_report, telemetry_report
from picoforge.dashboard.boards import DashboardBoards
from picoforge.dashboard.monitor import BoardConflict, CommandOutcomeUncertain, DeviceMonitor
from picoforge.dashboard.server import create_app
from picoforge.reports import ControlOpcode, ProtocolError


class CommandInvariantTests(unittest.TestCase):
    def ready(self, *, cleanup=True):
        client = test_device.HidDeviceTests().connection()
        client._expected_serial_number = None
        client.identity["capabilities"] |= 0x82
        client._device.feature_reports[2] = [configuration_report()]
        configuration = client.read_configuration()
        monitor = make_monitor(client, {
            "connected": True, "hardware": client.identity, "settings_supported": True,
            "board_configuration": configuration,
            "memory": {"msc": {"enabled": False, "transition_pending": False}},
            "self_test": {"active": False},
        })
        if cleanup:
            self.addCleanup(monitor.close)
        return client, monitor

    def test_led_publication_finishes_before_the_next_command_can_own_the_board(self):
        client, monitor = self.ready()
        client._device.feature_reports[20] = [
            test_device.control_result(transaction=1), test_device.control_result(transaction=2),
        ]
        monitor._snapshot["runtime"] = {"led_on": False}
        released, resume = threading.Event(), threading.Event()
        original = monitor._device
        errors = []

        @contextmanager
        def ownership():
            with original():
                yield
            if threading.current_thread().name == "first-led":
                released.set()
                if not resume.wait(3):
                    raise AssertionError("LED publication release timed out")

        def first():
            try:
                monitor.set_led(True)
            except Exception as error:
                errors.append(error)

        monitor._device = ownership
        worker = threading.Thread(target=first, name="first-led")
        worker.start()
        try:
            self.assertTrue(released.wait(2))
            self.assertTrue(monitor.snapshot()["runtime"]["led_on"], "publish before releasing ownership")
            monitor.set_led(False)
        finally:
            resume.set()
            worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual([bool(payload[5]) for payload in client._device.sent], [True, False])
        self.assertFalse(monitor.snapshot()["runtime"]["led_on"])

    def test_connection_publication_owns_the_client_and_initial_snapshot_together(self):
        client, monitor = self.ready()
        client._device.close = Mock()
        client._device.feature_reports[1] = [identity_report(capabilities=0x37)]
        client._device.feature_reports[2] = [configuration_report()]
        client._device.feature_reports[20] = [test_device.control_result(opcode=ControlOpcode.REBOOT)]
        monitor._active_client = None
        monitor._reset_snapshot()
        entered, release = threading.Event(), threading.Event()
        reset = monitor._reset_snapshot
        errors = []

        def publishing(**fields):
            if fields.get("connected"):
                entered.set()
                if not release.wait(3):
                    raise AssertionError("publication release timed out")
            reset(**fields)

        def command():
            try:
                monitor.reboot()
            except Exception as error:
                errors.append(error)

        monitor._reset_snapshot = publishing
        monitor._thread.start()
        request = threading.Thread(target=command)
        try:
            self.assertTrue(entered.wait(2))
            acquired = monitor._device_lock.acquire(blocking=False)
            if acquired:
                monitor._device_lock.release()
            request.start()
            release.set()
            request.join(3)
            self.assertFalse(acquired, "initial publication must retain command ownership")
            self.assertFalse(request.is_alive())
            self.assertEqual(errors, [])
            self.assertTrue(monitor.snapshot()["reboot_pending"])
            with self.assertRaises(BoardConflict):
                monitor.set_led(True)
            self.assertEqual([payload[3] for payload in client._device.sent], [ControlOpcode.REBOOT])
        finally:
            release.set()
            if request.ident is not None:
                request.join(3)
            monitor.close()

    def test_disconnect_publication_removes_the_client_under_command_ownership(self):
        client, monitor = self.ready()
        client._device.close = Mock()
        client._device.feature_reports[1] = [identity_report(capabilities=0x37)]
        client._device.feature_reports[2] = [configuration_report()]
        client.read_input_report = Mock(side_effect=OSError("detached"))
        entered, release = threading.Event(), threading.Event()
        reset = monitor._reset_snapshot

        def publishing(**fields):
            if fields.get("error") == "detached":
                entered.set()
                if not release.wait(3):
                    raise AssertionError("disconnect release timed out")
            reset(**fields)

        monitor._reset_snapshot = publishing
        monitor._thread.start()
        try:
            self.assertTrue(entered.wait(2))
            acquired = monitor._device_lock.acquire(blocking=False)
            if acquired:
                monitor._device_lock.release()
            active = monitor._active_client
            monitor._stop.set()
            release.set()
            monitor._thread.join(3)
            self.assertFalse(acquired, "disconnect publication must retain command ownership")
            self.assertIsNone(active)
            self.assertFalse(monitor._thread.is_alive())
            self.assertFalse(monitor.snapshot()["connected"])
            self.assertEqual(monitor.snapshot()["error"], "detached")
            with self.assertRaises(BoardConflict):
                monitor.reboot()
            self.assertEqual(client._device.sent, [])
            client._device.close.assert_called_once()
        finally:
            release.set()
            monitor.close()

    def test_completed_save_validation_failures_keep_the_reboot_gate(self):
        for wireless, mismatch in ((False, "settings"), (True, "settings"), (True, "commit")):
            with self.subTest(wireless=wireless, mismatch=mismatch):
                client, monitor = self.ready()
                board = {"system_clock_mhz": 150, "sdcard_profile": 1, "radios": 0, "tcp_cli_enabled": False}
                if wireless:
                    monitor._snapshot["self_test"] = None
                    monitor._snapshot["hardware"]["self_test_supported"] = False
                    monitor._snapshot["hardware"]["wireless_supported"] = True
                    client._device.feature_reports[12] = [test_wireless.status(), test_wireless.status()]
                saved_clock = 200 if mismatch == "settings" else 150
                result = bytearray(test_device.control_result(opcode=ControlOpcode.SAVE_SETTINGS))
                result[6:8] = saved_clock.to_bytes(2, "little")
                result[10] = 1 if mismatch == "commit" else 0
                client._device.feature_reports[20] = [result]
                initial_configuration = monitor.snapshot()["board_configuration"].copy()
                boards = Mock()
                boards.monitor.return_value = monitor
                app = create_app(boards)
                self.addCleanup(app.extensions["picoforge_console"].close_all)
                http = app.test_client()
                payload = ({"wifi_enabled": False, "ble_enabled": False, "confirm_reboot": True, "board": board}
                           if wireless else board)
                response = http.post("/api/" + ("wireless" if wireless else "settings") + "?board=a", json=payload)
                self.assertEqual(response.status_code, 503)
                snapshot = monitor.snapshot()
                self.assertTrue(snapshot["reboot_pending"])
                self.assertFalse(snapshot["command_outcome_uncertain"])
                self.assertEqual(snapshot["control_result"]["status"], "completed")
                self.assertEqual(snapshot["board_configuration"], initial_configuration)
                for route, payload in (("led", {"on": True}), ("reboot", {"confirm": True}), ("settings", board)):
                    self.assertEqual(http.post("/api/" + route + "?board=a", json=payload).status_code, 409)
                self.assertEqual(len(client._device.sent), 1)

    def test_refused_or_uncertain_save_and_reboot_do_not_claim_completion(self):
        for opcode in (ControlOpcode.SAVE_SETTINGS, ControlOpcode.REBOOT):
            for outcome in ("refused", "failed", "uncertain"):
                with self.subTest(opcode=opcode, outcome=outcome):
                    client, monitor = self.ready()
                    if outcome == "uncertain":
                        client._device.get_feature_report = Mock(side_effect=OSError("lost result"))
                        expected = CommandOutcomeUncertain
                    else:
                        client._device.feature_reports[20] = [test_device.control_result(
                            opcode=opcode, status=3 if outcome == "refused" else 6,
                            error=1 if outcome == "refused" else 6)]
                        expected = BoardConflict
                    command = (lambda: monitor.set_settings({
                        "system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0, "tcp_cli_enabled": False,
                    })) if opcode == ControlOpcode.SAVE_SETTINGS else monitor.reboot
                    with self.assertRaises(expected):
                        command()
                    self.assertFalse(monitor.snapshot()["reboot_pending"])
                    self.assertEqual(monitor.snapshot()["command_outcome_uncertain"], outcome == "uncertain")
                    self.assertEqual(len(client._device.sent), 1)

    def test_failed_replacement_cleanup_stops_the_monitor_and_is_reported_at_shutdown(self):
        client, monitor = self.ready(cleanup=False)
        client._device.close = Mock()
        client.identity["capabilities"] |= 64
        client._device.get_feature_report = Mock(side_effect=OSError("detached"))
        replacement = Mock()
        replacement.get_feature_report.return_value = bytes(27)
        replacement.close.side_effect = OSError("replacement close failed")
        with patch("picoforge.hid.device._matching_devices", return_value=[
                {"path": b"/hid/new", "serial_number": "0123456789ABCDEF"}]), \
                patch("picoforge.hid.device._require_hid") as hid, patch("picoforge.hid.device.sleep"):
            hid.return_value.device.return_value = replacement
            monitor._snapshot["memory"]["msc"]["enabled"] = True
            with self.assertRaises(CommandOutcomeUncertain):
                monitor.set_settings({"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0,
                                      "tcp_cli_enabled": False}, force=True)
            self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
            with self.assertRaises(BoardConflict):
                monitor.set_led(True)
            factory = Mock(return_value=client)
            monitor._client_factory = factory
            with self.assertLogs("picoforge.dashboard.monitor", "ERROR"):
                monitor._run()
            self.assertTrue(monitor._stop.is_set())
            self.assertIs(monitor._failed_client, client)
            self.assertIs(client._device, replacement)
            self.assertFalse(monitor.snapshot()["connected"])
            self.assertIn("cleanup failed", monitor.snapshot()["error"])
            with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
                monitor.close()
            factory.assert_called_once()
            hid.return_value.device.assert_called_once()
            replacement.close.assert_called_once()
            replacement.send_feature_report.assert_not_called()

    def test_confirmed_save_and_reboot_block_every_further_command(self):
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0, "tcp_cli_enabled": False}
        for opcode in (ControlOpcode.SAVE_SETTINGS, ControlOpcode.REBOOT):
            with self.subTest(opcode=opcode):
                client, monitor = self.ready()
                client._device.feature_reports[20] = [test_device.control_result(opcode=opcode)]
                if opcode == ControlOpcode.SAVE_SETTINGS:
                    monitor.set_settings(settings)
                else:
                    monitor.reboot()
                self.assertTrue(monitor.snapshot()["reboot_pending"])
                commands = [
                    lambda: monitor.set_led(True), lambda: monitor.set_msc(True),
                    lambda: monitor.set_settings(settings), lambda: monitor.reboot(),
                    lambda: monitor.set_self_test("start", 1), lambda: monitor.set_self_test("cancel"),
                    lambda: monitor.scan_wireless(), lambda: monitor.test_wireless("Pico", "password"),
                    lambda: monitor.cancel_wireless_test(), lambda: monitor.set_wireless({}),
                    lambda: monitor.tcp_cli_auth_proof(bytes(16), bytes(8)),
                ]
                for command in commands:
                    with self.assertRaises(BoardConflict):
                        command()
                self.assertFalse(monitor.resync_command()["uncertain"])
                boards = Mock()
                boards.monitor.return_value = monitor
                app = create_app(boards)
                self.addCleanup(app.extensions["picoforge_console"].close_all)
                http = app.test_client()
                for route, body in (
                    ("led", {"on": True}), ("msc", {"enabled": True}), ("settings", settings),
                    ("reboot", {"confirm": True}), ("self-test", {"action": "start", "size_mib": 1}),
                    ("wireless/test", {"ssid": "Pico", "password": "password"}),
                ):
                    self.assertEqual(http.post("/api/" + route + "?board=a", json=body).status_code, 409)
                self.assertEqual(len(client._device.sent), 1)

    def test_reconciled_save_or_reboot_retains_the_write_gate_only_on_completion(self):
        for opcode, status, pending in (
            (ControlOpcode.SAVE_SETTINGS, 2, True), (ControlOpcode.REBOOT, 2, True),
            (ControlOpcode.LED, 2, False), (ControlOpcode.SAVE_SETTINGS, 3, False),
        ):
            with self.subTest(opcode=opcode, status=status):
                client, monitor = self.ready()
                monitor._command_outcome_uncertain = True
                monitor._uncertain_command = (1, opcode)
                client._device.feature_reports[20] = [test_device.control_result(
                    opcode=opcode, status=status, error=1 if status == 3 else 0)]
                self.assertFalse(monitor.resync_command()["uncertain"])
                self.assertEqual(monitor.snapshot()["reboot_pending"], pending)
                if pending:
                    with self.assertRaises(BoardConflict):
                        monitor.set_led(True)
                self.assertEqual(client._device.sent, [])

    def test_real_reconnection_resets_pending_and_restores_writes(self):
        client, monitor = self.ready()
        client._device.feature_reports[20] = [test_device.control_result(opcode=ControlOpcode.REBOOT)]
        monitor.reboot()
        self.assertTrue(monitor.snapshot()["reboot_pending"])
        client._device.feature_reports[1] = [identity_report(capabilities=0x37)]
        client._device.feature_reports[2] = [configuration_report()]
        client._device.feature_reports[20] = [test_device.control_result(transaction=2)]
        client._device.close = Mock()
        client._device.reports = [telemetry_report(), storage_report()]
        monitor._client_factory = lambda: client

        def reconnected(_timeout):
            if monitor._stop.is_set():
                return True
            self.assertFalse(monitor.snapshot()["reboot_pending"])
            monitor.set_led(True)
            monitor._stop.set()
            return True

        monitor._stop.wait = reconnected
        monitor._run()
        self.assertEqual([payload[3] for payload in client._device.sent], [2, 1])

    def test_duplicate_identity_quarantines_retained_references_but_not_unrelated_boards(self):
        client, monitor = self.ready()
        attached = [{"serial_number": "same", "device_path": "/hid/a"}]
        factory = Mock(return_value=monitor)
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=factory)
        self.addCleanup(boards.close)
        board_id = boards.boards()["boards"][0]["id"]
        self.assertIs(boards.monitor(board_id), monitor)
        monitor._command_outcome_uncertain = True
        monitor._uncertain_command = (1, 1)
        attached.extend([{"serial_number": "same", "device_path": "/hid/b"},
                         {"serial_number": "other", "device_path": "/hid/c"}])
        app = create_app(boards)
        self.addCleanup(app.extensions["picoforge_console"].close_all)
        http = app.test_client()
        listing = http.get("/api/boards")
        self.assertEqual(listing.status_code, 200)
        self.assertEqual([board["serial_number"] for board in listing.json["boards"]], ["other"])
        self.assertTrue(listing.json["warnings"])
        self.assertEqual(http.post("/api/led?board=" + board_id, json={"on": True}).status_code, 409)
        with self.assertRaises(BoardConflict):
            monitor.set_led(True)
        self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
        self.assertFalse(monitor.try_retire())
        self.assertEqual(client._device.sent, [])
        attached.pop(1)
        boards.boards()
        client._device.feature_reports[20] = [test_device.control_result()]
        self.assertIs(boards.monitor(board_id), monitor)
        factory.assert_called_once()
        self.assertFalse(monitor.resync_command()["uncertain"])
        client._device.feature_reports[20] = [test_device.control_result()]
        monitor.set_led(True)
        self.assertEqual(len(client._device.sent), 1)

    def test_absence_cannot_clear_quarantine_until_positive_unique_discovery(self):
        client, monitor = self.ready()
        attached = [{"serial_number": "same", "device_path": "/hid/a"}]
        factory = Mock(return_value=monitor)
        boards = DashboardBoards(discover=lambda: attached, monitor_factory=factory, clock=lambda: 0.0)
        self.addCleanup(boards.close)
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id)
        attached.append({"serial_number": "same", "device_path": "/hid/b"})
        boards.boards()
        app = create_app(boards)
        self.addCleanup(app.extensions["picoforge_console"].close_all)
        http = app.test_client()

        for devices in ([], [{"serial_number": "other", "device_path": "/hid/c"}], []):
            attached[:] = devices
            listing = http.get("/api/boards")
            self.assertEqual(listing.status_code, 200)
            self.assertTrue(listing.json["warnings"])
            self.assertEqual(http.post("/api/led?board=" + board_id, json={"on": True}).status_code, 409)
            with self.assertRaises(BoardConflict):
                boards.monitor(board_id)
            with self.assertRaises(BoardConflict):
                monitor.set_led(True)
            self.assertEqual(client._device.sent, [])

        attached[:] = [{"serial_number": "same", "device_path": "/hid/new"}]
        self.assertNotIn("warnings", http.get("/api/boards").json)
        self.assertIs(boards.monitor(board_id), monitor)
        client._device.feature_reports[20] = [test_device.control_result()]
        self.assertEqual(http.post("/api/led?board=" + board_id, json={"on": True}).status_code, 200)
        self.assertEqual(len(client._device.sent), 1)
        factory.assert_called_once()

    def test_failed_discovery_cannot_clear_identity_quarantine(self):
        client, monitor = self.ready()
        attached = [{"serial_number": "same", "device_path": "/hid/a"}]
        discover = Mock(return_value=attached)
        factory = Mock(return_value=monitor)
        boards = DashboardBoards(discover=discover, monitor_factory=factory)
        self.addCleanup(boards.close)
        board_id = boards.boards()["boards"][0]["id"]
        boards.monitor(board_id)
        attached.append({"serial_number": "same", "device_path": "/hid/b"})
        boards.boards()
        discover.side_effect = OSError("enumeration failed")
        with self.assertRaisesRegex(OSError, "enumeration failed"):
            boards.boards()
        with self.assertRaises(BoardConflict):
            boards.monitor(board_id)
        with self.assertRaises(BoardConflict):
            monitor.set_led(True)
        self.assertEqual(client._device.sent, [])
        discover.side_effect = None
        attached.pop()
        boards.boards()
        self.assertIs(boards.monitor(board_id), monitor)
        factory.assert_called_once()
        client._device.feature_reports[20] = [test_device.control_result()]
        monitor.set_led(True)
        self.assertEqual(len(client._device.sent), 1)

    def test_identity_quarantine_does_not_interrupt_an_accepted_command(self):
        client, monitor = self.ready()
        entered, release = threading.Event(), threading.Event()
        errors = []
        send = client._device.send_feature_report

        def sending(payload):
            entered.set()
            if not release.wait(2):
                raise AssertionError("command release timed out")
            return send(payload)

        def command():
            try:
                monitor.set_led(True)
            except Exception as error:
                errors.append(error)

        client._device.send_feature_report = sending
        client._device.feature_reports[20] = [test_device.control_result()]
        worker = threading.Thread(target=command)
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            monitor.set_identity_ambiguous(True)
            release.set()
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors, [])
            with self.assertRaises(BoardConflict):
                monitor.set_led(False)
            self.assertEqual(len(client._device.sent), 1)
        finally:
            release.set()
            worker.join(2)


class WirelessCleanupInvariantTests(unittest.TestCase):
    def ready(self, cancel_result, *, late=True):
        client = test_wireless.WirelessTests().client([
            test_wireless.status(), test_wireless.result(6, 1), test_wireless.result(7, 2),
            test_wireless.result(9, 3),
            test_wireless.status(wifi=5, flags=0xB3, test=1, staged=3), cancel_result,
        ])
        client._device.read.return_value = []
        monitor = make_monitor(client, {
            "connected": True, "hardware": {"wireless_supported": True, "self_test_supported": False,
                                           "led_available": True},
            "memory": {"msc": {"enabled": False, "transition_pending": False}},
        })
        boards = Mock()
        boards.monitor.return_value = monitor
        app = create_app(boards)
        self.addCleanup(app.extensions["picoforge_console"].close_all)
        original = client.test_wireless

        def finish_then_cancel(*args, **kwargs):
            if not late:
                # The request arrives during the first poll, before the native test can finish.
                kwargs["on_poll"] = lambda: monitor.cancel_wireless_test()
                return original(*args, **kwargs)
            answer = original(*args, **kwargs)
            self.assertEqual(monitor.cancel_wireless_test(), {"cancelled": True})
            return answer

        client.test_wireless = finish_then_cancel
        return client, monitor, app.test_client()

    def test_known_late_cancel_failure_requires_explicit_cleanup_not_unresolvable_resync(self):
        client, monitor, http = self.ready(test_wireless.result(10, 4, state=6, error=6))
        response = http.post("/api/wireless/test?board=a", json={"ssid": "Pico", "password": "password"})
        self.assertEqual(response.status_code, 409)
        snapshot = monitor.snapshot()
        self.assertTrue(snapshot["wireless_cleanup_required"])
        self.assertFalse(snapshot["command_outcome_uncertain"])
        self.assertFalse(client.command_uncertain)
        self.assertFalse(monitor.try_retire(), "cleanup ownership must survive discovery retirement")
        self.assertEqual(http.post("/api/led?board=a", json={"on": True}).status_code, 409)
        self.assertEqual(http.post("/api/control/resync?board=a", json={"confirm": True}).status_code, 200)
        self.assertTrue(monitor.snapshot()["wireless_cleanup_required"], "Re-sync is not cleanup")
        self.assertEqual([call.args[0][3] for call in client._device.send_feature_report.call_args_list], [6, 7, 9, 10])

        client._device.get_feature_report.side_effect = [
            test_wireless.result(10, 5, state=6, error=6), test_wireless.result(10, 6), test_wireless.result(1, 7),
        ]
        self.assertEqual(http.post("/api/wireless/cancel?board=a").status_code, 409)
        self.assertTrue(monitor.snapshot()["wireless_cleanup_required"])
        self.assertEqual(http.post("/api/wireless/cancel?board=a").status_code, 200)
        self.assertFalse(monitor.snapshot()["wireless_cleanup_required"])
        self.assertEqual(http.post("/api/led?board=a", json={"on": True}).status_code, 200)
        self.assertEqual([call.args[0][3] for call in client._device.send_feature_report.call_args_list],
                         [6, 7, 9, 10, 10, 10, 1])

    def test_unknown_late_cancel_keeps_exact_reference_and_reconciles_without_resending(self):
        client, monitor, http = self.ready(OSError("lost cancellation reply"))
        response = http.post("/api/wireless/test?board=a", json={"ssid": "Pico", "password": "password"})
        self.assertEqual(response.status_code, 503)
        self.assertTrue(response.json["uncertain"])
        self.assertTrue(monitor.snapshot()["wireless_cleanup_required"])
        self.assertEqual(monitor._uncertain_command, (4, ControlOpcode.WIFI_CANCEL))
        client._device.get_feature_report.side_effect = [test_wireless.result(10, 4)]
        self.assertEqual(http.post("/api/wireless/cancel?board=a").status_code, 409)
        self.assertEqual(http.post("/api/control/resync?board=a", json={"confirm": True}).status_code, 200)
        self.assertFalse(monitor.snapshot()["wireless_cleanup_required"])
        self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])
        self.assertEqual(client._device.send_feature_report.call_count, 4)

    def test_running_test_known_cancel_failure_is_not_automatically_retried(self):
        client, monitor, http = self.ready(test_wireless.result(10, 4, state=6, error=6), late=False)
        # A running cancellation is sent instead of reading the final joined state.
        client._device.get_feature_report.side_effect = [
            test_wireless.status(), test_wireless.result(6, 1), test_wireless.result(7, 2),
            test_wireless.result(9, 3), test_wireless.status(wifi=4, test=1),
            test_wireless.result(10, 4, state=6, error=6),
        ]
        response = http.post("/api/wireless/test?board=a", json={"ssid": "Pico", "password": "password"})
        self.assertEqual(response.status_code, 409)
        self.assertTrue(monitor.snapshot()["wireless_cleanup_required"])
        self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])
        self.assertEqual([call.args[0][3] for call in client._device.send_feature_report.call_args_list], [6, 7, 9, 10])

    def test_failed_cancel_reconciliation_keeps_cleanup_until_an_explicit_success(self):
        client, monitor, http = self.ready(OSError("lost cancellation reply"))
        self.assertEqual(http.post("/api/wireless/test?board=a",
                                   json={"ssid": "Pico", "password": "password"}).status_code, 503)
        client._device.get_feature_report.side_effect = [
            test_wireless.result(10, 4, state=6, error=6), test_wireless.result(10, 5),
        ]
        self.assertEqual(http.post("/api/control/resync?board=a", json={"confirm": True}).status_code, 200)
        self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])
        self.assertTrue(monitor.snapshot()["wireless_cleanup_required"])
        self.assertEqual(client._device.send_feature_report.call_count, 4)
        self.assertEqual(http.post("/api/wireless/cancel?board=a").status_code, 200)
        self.assertFalse(monitor.snapshot()["wireless_cleanup_required"])
        self.assertEqual(client._device.send_feature_report.call_count, 5)


class ConfigurationInvariantTests(unittest.TestCase):
    def test_expected_configuration_failures_preserve_diagnostics_and_block_dependent_writes(self):
        for failure in (OSError("Feature 2 unavailable"), ProtocolError("malformed Feature 2")):
            with self.subTest(failure=failure):
                client = Mock()
                client.read_info.return_value = {
                    "supported": True, "settings_supported": True, "wireless_supported": False,
                    "led_available": True,
                }
                client.read_configuration.side_effect = failure
                client.read_input_report.return_value = None
                client.set_led.return_value = {"status": "completed", "error": "none"}
                client.command_uncertain = False
                monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)

                def check(_timeout):
                    if monitor._stop.is_set():
                        return True
                    status = monitor.snapshot()
                    self.assertTrue(status["connected"])
                    self.assertIsNone(status["error"])
                    self.assertIn(str(failure), status["settings_message"])
                    monitor.set_led(True)
                    for command in (lambda: monitor.set_msc(True), lambda: monitor.reboot(),
                                    lambda: monitor.set_self_test("start", 1)):
                        with self.assertRaises(BoardConflict):
                            command()
                    self.assertFalse(monitor.resync_command()["uncertain"])
                    monitor._stop.set()
                    return True

                monitor._stop.wait = check
                with self.assertLogs("picoforge.dashboard.monitor", "WARNING"):
                    monitor._run()
                self.assertTrue(monitor.snapshot()["connected"])
                self.assertIsNone(monitor.snapshot()["error"])
                client.set_led.assert_called_once()
                client.set_msc.assert_not_called()
                client.reboot.assert_not_called()

    def test_configuration_programming_fault_is_logged_as_internal_not_degraded_success(self):
        for failure in (ValueError("parser bug"), RuntimeError("configuration bug")):
            with self.subTest(failure=failure):
                client = Mock()
                client.read_info.return_value = {"supported": True}
                client.read_configuration.side_effect = failure
                monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)
                monitor._stop.wait = lambda _timeout: monitor._stop.set()
                with self.assertLogs("picoforge.dashboard.monitor", "ERROR"):
                    monitor._run()
                status = monitor.snapshot()
                self.assertFalse(status["connected"])
                self.assertTrue(status["error"].startswith("Internal monitor error:"))
                self.assertIsNone(status["settings_message"])
                client.read_input_report.assert_not_called()
