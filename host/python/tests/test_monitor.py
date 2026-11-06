import sys
import threading
import time
import unittest
from unittest.mock import Mock, patch

from helpers import make_monitor
from picoforge.dashboard.monitor import CommandOutcomeUncertain, DeviceMonitor, _empty_snapshot
from picoforge.reports import ProtocolError


class MonitorMscTests(unittest.TestCase):
    def setUp(self):
        self.client = Mock()
        self.monitor = make_monitor(self.client, {"connected": True, "memory": {"msc": {"enabled": True}}})
        self.client.set_msc.return_value = {"status": "completed", "error": "none"}
        self.client.set_led.return_value = {"status": "completed", "error": "none"}

    def test_forced_settings_require_confirmed_idle_host_ownership(self):
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0}
        snapshot = {**_empty_snapshot(), "connected": True, "settings_supported": True,
                    "hardware": {"capabilities": 68, "storage_transport": "sdio"},
                    "memory": {"msc": {"enabled": True, "transition_pending": False}},
                    "self_test": {"active": False}}
        self.monitor._snapshot = dict(snapshot)
        self.client.set_settings.return_value = settings
        self.assertEqual(self.monitor.set_settings(settings, force=True)["settings"], settings)
        self.client.set_settings.assert_called_once_with(settings, force=True)
        for mutate in [
            {"memory": {"msc": {"enabled": False, "transition_pending": False}}},
            {"memory": {"msc": {"enabled": True, "transition_pending": True}}},
            {"self_test": {"active": True}},
        ]:
            self.monitor._snapshot = {**snapshot, **mutate}
            self.client.reset_mock()
            with self.assertRaises(RuntimeError):
                self.monitor.set_settings(settings, force=True)
            self.client.set_settings.assert_not_called()
        self.monitor._snapshot = snapshot
        with self.assertRaisesRegex(RuntimeError, "Disable"):
            self.monitor.set_settings(settings)

    def test_led_disconnect_after_command_preserves_disconnected_snapshot(self):
        self.monitor._snapshot = {**_empty_snapshot(), "connected": True,
                                  "hardware": {"led_available": True},
                                  "runtime": {"led_available": True, "led_on": False}}

        def disconnect(on):
            with self.monitor._lock:
                self.monitor._snapshot = _empty_snapshot()
            return {"status": "completed", "error": "none"}

        self.client.set_led.side_effect = disconnect
        self.assertEqual(self.monitor.set_led(True), {"control_result": {"status": "completed", "error": "none"}})
        self.client.set_led.assert_called_once_with(True)
        snapshot = self.monitor.snapshot()
        self.assertFalse(snapshot["connected"])
        self.assertIsNone(snapshot["control_result"])

    def test_led_on_disconnected_board_is_unavailable_not_conflict(self):
        self.monitor._active_client = None
        with self.assertRaises(ConnectionError):
            self.monitor.set_led(True)
        self.monitor._active_client = self.client
        self.monitor._snapshot = {**_empty_snapshot(), "connected": True, "hardware": {"led_available": False}}
        with self.assertRaisesRegex(RuntimeError, "The board LED is unavailable"):
            self.monitor.set_led(True)
        self.client.set_led.assert_not_called()

    def test_confirmed_return_uses_only_hid_on_each_host_platform(self):
        for platform in ("linux", "win32", "darwin"):
            with self.subTest(platform=platform), patch.object(sys, "platform", platform), \
                    patch("subprocess.run", side_effect=AssertionError("Host eject must not run")):
                self.monitor._snapshot["memory"]["msc"]["transition_pending"] = False
                self.client.reset_mock()
                status = self.monitor.set_msc(False, force=True)["msc"]
                self.client.set_msc.assert_called_once_with(False, force=True)
                self.assertTrue(status["transition_pending"])

    def test_safe_return_uses_only_hid(self):
        self.monitor.set_msc(False)
        self.client.set_msc.assert_called_once_with(False, force=False)

    def test_force_return_requires_enabled_idle_msc(self):
        for field, value in (("enabled", False), ("transition_pending", True)):
            with self.subTest(field=field):
                self.monitor._snapshot["memory"]["msc"] = {"enabled": True, "transition_pending": False, field: value}
                with self.assertRaisesRegex(RuntimeError, "requires enabled MSC"):
                    self.monitor.set_msc(False, force=True)
                self.client.set_msc.assert_not_called()

    def test_enable_keeps_existing_command(self):
        self.monitor.set_msc(True)
        self.client.set_msc.assert_called_once_with(True, force=False)
        self.assertEqual(self.monitor._msc_reconnect_deadline, 0.0)
        self.assertIsNone(self.monitor._msc_reconnect_enabled)

    def test_reboot_exposes_control_result(self):
        result = {"status": "completed", "opcode": 2, "transaction_id": 1}
        self.client.reboot.return_value = result
        self.monitor._snapshot = {**_empty_snapshot(), "connected": True}
        self.assertEqual(self.monitor.reboot(), {"control_result": result})
        self.assertEqual(self.monitor.snapshot()["control_result"], result)

    def test_uncertain_command_blocks_writes_even_after_client_reconnects(self):
        self.monitor._snapshot = {
            **_empty_snapshot(),
            "connected": True,
            "hardware": {"led_available": True},
            "runtime": {"led_available": True, "led_on": False},
            "memory": {"msc": {"enabled": False, "transition_pending": False}},
        }
        self.client.command_uncertain = True
        self.client.set_led.side_effect = TimeoutError("uncertain")
        with self.assertRaises(CommandOutcomeUncertain):
            self.monitor.set_led(True)
        self.assertTrue(self.monitor.snapshot()["command_outcome_uncertain"])
        self.client = self.monitor._active_client = Mock(command_uncertain=False)
        with self.assertRaisesRegex(RuntimeError, "outcome is uncertain"):
            self.monitor.set_msc(True)
        self.client.set_msc.assert_not_called()

    def test_resync_keeps_uncertainty_until_exact_result_is_confirmed(self):
        client = Mock(command_uncertain=True)
        monitor = make_monitor(client, {"connected": True})
        monitor._command_outcome_uncertain = True
        monitor._uncertain_command = (7, 11)
        monitor._uncertain_wireless_operation = ("scan", 3)
        client.reconcile_outcome.return_value = None
        with self.assertRaises(CommandOutcomeUncertain):
            monitor.resync_command()
        self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])
        client.reconcile_outcome.assert_called_once_with((7, 11), ("scan", 3))

        control_result = {"transaction_id": 7, "opcode": 11, "status": "completed"}
        wireless = {"scan_generation": 3, "scan_state": "complete"}
        client.reconcile_outcome.return_value = control_result, wireless
        self.assertEqual(monitor.resync_command(), {
            "uncertain": False, "control_result": control_result, "wireless": wireless,
        })
        self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])
        self.assertEqual(monitor.snapshot()["wireless"], wireless)

    def test_malformed_input_report_does_not_abort_wireless_poll_drain(self):
        client = Mock(command_uncertain=False)
        client.read_input_report.side_effect = [ProtocolError("bad report"), None]
        monitor = make_monitor(client)
        with self.assertLogs("picoforge.dashboard.monitor", "WARNING"):
            monitor._drain_input_reports()
        self.assertFalse(monitor.snapshot()["command_outcome_uncertain"])
        self.assertEqual(client.read_input_report.call_count, 2)

    def test_snapshot_mutations_do_not_change_monitor_state(self):
        monitor = make_monitor(snapshot={"hardware": {"product_name": "PicoForge"}, "runtime": {"uptime_ms": 42}})

        snapshot = monitor.snapshot()
        snapshot["hardware"]["product_name"] = "changed"
        snapshot["runtime"]["uptime_ms"] = 0

        self.assertEqual(monitor._snapshot["hardware"]["product_name"], "PicoForge")
        self.assertEqual(monitor._snapshot["runtime"]["uptime_ms"], 42)


class MonitorReconnectTests(unittest.TestCase):
    def test_native_close_failure_stops_reconnect_and_is_reported_by_shutdown(self):
        client = Mock()
        client.read_info.return_value = {"supported": False, "product_name": "Other"}
        client.close.side_effect = OSError("native close failed")
        factory = Mock(return_value=client)
        monitor = DeviceMonitor(client_factory=factory, autostart=False)
        with self.assertLogs("picoforge.dashboard.monitor", "ERROR"):
            monitor._run()
        self.assertTrue(monitor._stop.is_set())
        self.assertIs(monitor._failed_client, client)
        self.assertFalse(monitor.snapshot()["connected"])
        self.assertIn("cleanup failed", monitor.snapshot()["error"])
        with self.assertRaisesRegex(RuntimeError, "HID cleanup failed") as error:
            monitor.close()
        self.assertIs(error.exception.__cause__, client.close.side_effect)
        factory.assert_called_once()
        client.close.assert_called_once()

    def test_unsupported_product_backs_off_before_reopening(self):
        client = Mock()
        client.read_info.return_value = {"supported": False, "product_name": "Other"}
        factory = Mock(return_value=client)
        monitor = DeviceMonitor(client_factory=factory)
        time.sleep(0.3)
        monitor.close()
        self.assertEqual(factory.call_count, 1, "unsupported firmware must not reopen HID in a tight loop")
        client.close.assert_called_once_with()
        self.assertEqual(monitor.snapshot()["error"], "Unsupported HID product: Other")

    def test_unexpected_report_error_keeps_the_monitor_alive_and_visible(self):
        client = Mock()
        client.read_info.return_value = {
            "supported": True, "settings_supported": False, "wireless_supported": False,
        }
        client.read_input_report.return_value = {"kind": "memory", "data": {}}
        with self.assertLogs("picoforge.dashboard.monitor", "ERROR"):
            monitor = DeviceMonitor(client_factory=lambda: client)
            time.sleep(0.3)
        snapshot = monitor.snapshot()
        alive = monitor._thread.is_alive()
        monitor.close()
        self.assertTrue(alive, "an unexpected error must not end the monitor thread")
        self.assertFalse(snapshot["connected"])
        self.assertRegex(snapshot["error"], r"^Internal monitor error: KeyError\('msc'\)")

    def test_monitor_yields_outside_the_hid_lock_between_nonblocking_input_batches(self):
        monitor = make_monitor(fresh=False)
        client = Mock()
        client.read_info.return_value = {
            "supported": True, "settings_supported": False, "wireless_supported": False,
        }
        client.read_configuration.return_value = {
            "system_clock_mhz": 0, "sdcard_profile": 0, "radios": 0, "tcp_cli_enabled": False,
        }
        client.read_input_report.side_effect = lambda timeout_ms: monitor._stop.set()
        monitor._client_factory = lambda: client
        waits = []
        monitor.status_snapshot()

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
            (ConnectionError("PicoForge HID interface not found; check USB"), 104.0, True, False, True),
            (OSError("read error"), 105.0, True, False, False),
            (OSError("read error"), 104.0, False, False, False),
            (OSError("read error"), 104.0, True, True, False),
            (ValueError("invalid report"), 104.0, True, False, False),
            (RuntimeError("unexpected failure"), 104.0, True, False, False),
        ]
        for error, now, confirmed, uncertain, reconnecting in cases:
            with self.subTest(error=error, now=now, confirmed=confirmed, uncertain=uncertain):
                monitor = make_monitor(Mock(), {"memory": {"msc": {"enabled": False}}})
                monitor._stop.wait = lambda timeout: monitor._stop.set()
                monitor._active_client.set_msc.return_value = {"status": "completed", "error": "none"}
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
                if isinstance(error, (ValueError, RuntimeError)):
                    self.assertTrue(snapshot["error"].startswith("Internal monitor error:"))
                else:
                    self.assertEqual(snapshot["error"], None if reconnecting else str(error))

    def test_reconnect_snapshot_keeps_stable_api_shape(self):
        monitor = make_monitor(fresh=False)
        client = Mock()
        client.read_info.return_value = {
            "supported": True, "product_name": "PicoForge", "settings_supported": True,
            "wireless_supported": False,
            "storage_transport": "sdio",
            "self_test_supported": True,
        }
        client.read_configuration.return_value = {
            "system_clock_mhz": 200, "sdcard_profile": 1, "stored_sysclk_profile": 200,
            "stored_sdcard_profile": 1, "radios": 0, "tcp_cli_enabled": False, "storage_transport": "sdio",
        }
        client.read_input_report.side_effect = lambda timeout_ms: monitor._stop.set()
        monitor._client_factory = lambda: client
        monitor._snapshot = {}
        monitor._run()
        snapshot = monitor.snapshot()
        self.assertTrue(snapshot["connected"])
        self.assertEqual(snapshot["board_configuration"], client.read_configuration.return_value)
        self.assertTrue(snapshot["settings_supported"])
        self.assertTrue(snapshot["telemetry_stale"] and snapshot["storage_stale"] and snapshot["self_test_stale"])
        self.assertEqual(set(snapshot), set(_empty_snapshot()))

    def test_storage_and_self_test_reports_publish_state_and_freshness(self):
        monitor = make_monitor(fresh=False)
        storage = {
            "filesystem_format": "fat32",
            "msc": {"enabled": False, "transition_pending": False, "request_rejected": False},
        }
        self_test = {"state": "idle", "active": False}
        client = Mock()
        client.read_info.return_value = {
            "supported": True, "product_name": "PicoForge", "settings_supported": False,
            "wireless_supported": False,
            "storage_transport": "sdio", "self_test_supported": True,
        }
        client.read_configuration.return_value = {
            "system_clock_mhz": 0,
            "sdcard_profile": 0,
            "radios": 0,
            "tcp_cli_enabled": False,
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
        monitor._run()

        snapshot = captured["snapshot"]
        self.assertEqual(snapshot["memory"], storage)
        self.assertEqual(snapshot["self_test"], self_test)
        self.assertFalse(snapshot["storage_stale"])
        self.assertFalse(snapshot["self_test_stale"])
        self.assertTrue(snapshot["telemetry_stale"], "never-received telemetry is stale")

    def test_input_freshness_is_tracked_and_expires_per_report(self):
        monitor = make_monitor(snapshot={
            "hardware": {"self_test_supported": True},
            "runtime": {"uptime_ms": 1},
            "memory": {"sample_valid": True},
            "self_test": {"active": True},
        })
        monitor._input_received_at = {"runtime": 99.5, "memory": 98.0, "self_test": 98.0}
        with patch("picoforge.dashboard.monitor.time.monotonic", return_value=100.0):
            snapshot = monitor.snapshot()
        self.assertFalse(snapshot["telemetry_stale"])
        self.assertTrue(snapshot["storage_stale"])
        self.assertTrue(snapshot["self_test_stale"])
        self.assertIsNone(snapshot["memory"])
        self.assertIsNone(snapshot["self_test"])
        self.assertEqual(snapshot["runtime"], {"uptime_ms": 1})


class MonitorWirelessCancellationTests(unittest.TestCase):
    def test_late_cancel_failure_does_not_replace_successful_test_result(self):
        client = Mock()
        snapshot = {
            "connected": True,
            "hardware": {"wireless_supported": True, "self_test_supported": False},
            "memory": {"msc": {"enabled": False, "transition_pending": False}},
            "storage_stale": False,
        }
        monitor = make_monitor(client, snapshot)
        expected = {"transaction_id": 1, "state": "completed"}

        def finish_test(_ssid, _password, *, cancel, **_kwargs):
            cancel.set()
            return expected

        client.test_wireless.side_effect = finish_test
        client.cancel_wireless_test.side_effect = OSError("USB disconnected during cleanup")

        with self.assertLogs("picoforge.dashboard.monitor", "ERROR"), \
                self.assertRaises(CommandOutcomeUncertain):
            monitor.test_wireless("network")
        client.cancel_wireless_test.assert_called_once_with()
        self.assertTrue(monitor.snapshot()["command_outcome_uncertain"])


class MonitorSettingsTests(unittest.TestCase):
    def setUp(self):
        self.client = Mock()
        self.monitor = make_monitor(self.client, {
            "connected": True,
            "hardware": {"storage_transport": "sdio"},
            "settings_supported": True,
            "board_configuration": {"system_clock_mhz": 0, "sdcard_profile": 0, "radios": 0, "reset_cause": 1},
            "memory": {"msc": {"enabled": False, "transition_pending": False}},
        })

    def test_saves_settings_only_when_supported_and_storage_is_idle(self):
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0}
        self.client.set_settings.return_value = settings
        self.client.configuration = settings
        self.client.last_command_result = None

        def save(*_args, **_kwargs):
            self.client.last_command_result = {"status": "completed", "opcode": 5}
            return settings

        self.client.set_settings.side_effect = save
        saved = self.monitor.set_settings(settings)["settings"]
        self.assertEqual(saved["sdcard_profile"], 1)
        self.client.set_settings.assert_called_once_with(settings, force=False)
        snapshot = self.monitor.snapshot()
        self.assertIn("Waiting for the board to reboot", snapshot["settings_message"])
        self.assertEqual(snapshot["board_configuration"], {**settings, "reset_cause": 1})
        self.assertTrue(snapshot["reboot_pending"])

    def test_canonical_settings_are_saved_and_returned_without_transport_fields(self):
        settings = {"system_clock_mhz": 150, "sdcard_profile": 0, "radios": 0}
        self.client.set_settings.return_value = settings
        self.client.configuration = {"system_clock_mhz": 150, "sdcard_profile": 0}
        self.assertEqual(self.monitor.set_settings(settings)["settings"], settings)
        self.client.set_settings.assert_called_once_with(settings, force=False)

    def test_failed_save_does_not_republish_an_older_command_result(self):
        older = {"opcode": 11, "status": "completed"}
        self.client.last_command_result = older
        self.client.set_settings.side_effect = ValueError("bad clock")
        with self.assertRaises(ValueError):
            self.monitor.set_settings({"system_clock_mhz": 1})
        self.assertIsNone(self.monitor.snapshot()["control_result"])

    def test_rejects_settings_when_storage_state_is_unknown_or_busy(self):
        settings = {"system_clock_mhz": 150, "sdcard_profile": 0, "radios": 0}
        for msc in (None, {"enabled": True, "transition_pending": False},
                    {"enabled": False, "transition_pending": True}):
            with self.subTest(msc=msc):
                self.monitor._snapshot["memory"] = None if msc is None else {"msc": msc}
                self.client.set_settings.reset_mock()
                with self.assertRaises(RuntimeError):
                    self.monitor.set_settings(settings)
                self.client.set_settings.assert_not_called()


class MonitorPollingTests(unittest.TestCase):
    def test_idle_polling_slows_and_status_request_restores_active_cadence(self):
        client = Mock()
        client.read_info.return_value = {
            "supported": True,
            "wireless_supported": True,
            "settings_supported": False,
            "self_test_supported": False,
        }
        client.read_configuration.return_value = {}
        client.read_input_report.return_value = None
        client.read_wireless.return_value = {"wifi_state": "connected", "stale": False}
        monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)
        clock = [100.0]
        waits = []
        monitor._last_status_request_at = 95.0

        def wait(timeout):
            waits.append(timeout)
            clock[0] += timeout
            if len(waits) == 2:
                monitor.status_snapshot()
            elif len(waits) == 5:
                monitor._stop.set()
            return monitor._stop.is_set()

        with patch("picoforge.dashboard.monitor.time.monotonic", side_effect=lambda: clock[0]):
            monitor._stop.wait = wait
            monitor._run()

        self.assertEqual(waits[:2], [0.5, 0.5])
        self.assertEqual(waits[2:5], [0.05, 0.05, 0.05])
        self.assertEqual(client.read_wireless.call_count, 2, "status polling resumes wireless refresh immediately")

    def test_idle_wireless_status_poll_interval_is_five_seconds(self):
        client = Mock()
        client.read_info.return_value = {
            "supported": True,
            "wireless_supported": True,
            "settings_supported": False,
            "self_test_supported": False,
        }
        client.read_configuration.return_value = {}
        client.read_input_report.return_value = None
        clock = [200.0]
        wireless_reads = []
        client.read_wireless.side_effect = lambda: wireless_reads.append(clock[0]) or {"stale": False}
        monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)
        monitor._last_status_request_at = 195.0
        waits = []

        def wait(timeout):
            waits.append(timeout)
            clock[0] += timeout
            if len(waits) == 11:
                monitor._stop.set()
            return monitor._stop.is_set()

        with patch("picoforge.dashboard.monitor.time.monotonic", side_effect=lambda: clock[0]):
            monitor._stop.wait = wait
            monitor._run()

        self.assertEqual(wireless_reads, [200.0, 205.0])


if __name__ == "__main__":
    unittest.main()
