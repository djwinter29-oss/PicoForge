import unittest
from unittest.mock import patch

from picoforge.dashboard.server import create_app
from picoforge.cli import parse_args


class FakeMonitor:
    def snapshot(self, board_id=None):
        self.last_board_id = board_id
        return {
            "connected": True,
            "error": None,
            "hardware": {
                "product_name": "PicoForge",
                "mcu": "RP2040",
                "mcu_id": 1,
                "serial_number": "0123456789abcdef",
                "storage_transport": "sdio",
                "led_available": True,
                "self_test_supported": True,
                "settings_supported": True,
            },
            "runtime": {
                "version": 1,
                "sequence": 7,
                "uptime_ms": 42_000,
                "system_clock_hz": 125_000_000,
                "usb_mounted": True,
                "cdc_open": False,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": False,
                "cpu_usage_core0_basis_points": 1234,
                "cpu_usage_core1_basis_points": 5678,
                "ram_used_bytes": 65536,
                "ram_total_bytes": 532480,
            },
            "board_status": {
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": False,
            },
            "msc": {"enabled": False, "transition_pending": False, "request_rejected": False},
            "settings": {"system_clock_mhz": 0, "sdcard_profile": 0, "radios": 0},
            "board_configuration": {
                "stored_sysclk_profile": 0, "stored_sdcard_profile": 0,
                "system_clock_mhz": 0, "sdcard_profile": 0, "radios": 0,
            },
            "settings_supported": True,
            "settings_message": None,
            "control_result": None,
            "report_freshness": {
                "3": {"available": True, "stale": False, "age_ms": 20},
                "10": {"available": True, "stale": False, "age_ms": 30},
                "11": {"supported": True, "available": False, "stale": False, "age_ms": None},
            },
            "telemetry_stale": False,
            "storage_stale": False,
            "self_test_stale": False,
            "updated_at": "2026-10-10T12:00:00+00:00",
        }

    def boards(self):
        return {"boards": [{"id": "board-a", "product_name": "PicoForge",
                            "serial_number": "0123456789abcdef", "device_path": "/hid/a"}],
                "default_board_id": "board-a"}

    def set_led(self, board_id, on):
        self.last_board_id = board_id
        return {"temperature_valid": True, "temperature_centidegrees": 2345,
                "led_available": True, "led_on": on}

    def set_msc(self, board_id, enabled, *, force=False):
        self.last_board_id = board_id
        self.last_msc = (enabled, force)
        return {"enabled": enabled, "transition_pending": True, "request_rejected": False}

    def set_settings(self, board_id, settings, *, force=False):
        self.last_board_id = board_id
        self.last_force_save = force
        self.last_settings = settings
        return settings

    def set_self_test(self, board_id, action, size_mib):
        self.last_board_id = board_id
        self.last_self_test = (action, size_mib)

    def reboot(self, board_id):
        self.last_board_id = board_id
        return {"status": "completed", "opcode": 2, "transaction_id": 1}


class FalseyMonitor(FakeMonitor):
    def __bool__(self) -> bool:
        return False


class WebTests(unittest.TestCase):
    def test_unreleased_pages_display_the_debug_app_version(self):
        client = create_app(FakeMonitor()).test_client()
        for route in ("/", "/console"):
            with self.subTest(route=route):
                page = client.get(route).get_data(as_text=True)
                self.assertIn("Python · App v0.0.0-debug<", page)

    def test_release_pages_display_the_stamped_app_version(self):
        with patch("picoforge.dashboard.server.__version__", "1.2.3"):
            client = create_app(FakeMonitor()).test_client()
            for route in ("/", "/console"):
                with self.subTest(route=route):
                    page = client.get(route).get_data(as_text=True)
                    self.assertIn("Python · App v1.2.3<", page)
                    self.assertNotIn("App v0.0.0-debug", page)

    def test_create_app_keeps_an_injected_falsey_dashboard(self):
        monitor = FalseyMonitor()

        app = create_app(monitor)

        self.assertIs(app.extensions["picoforge_monitor"], monitor)
    def test_server_dashboard_does_not_serve_browser_bluetooth_page(self):
        client = create_app(FakeMonitor()).test_client()
        self.assertEqual(client.get("/ble/").status_code, 404)
        self.assertEqual(client.get("/ble/js/app.mjs").status_code, 404)
        page = client.get("/").get_data(as_text=True)
        self.assertNotIn("/ble/", page)
        self.assertEqual(page.count('<option value="configure">Configure…</option>'), 2)
        self.assertIn('id="wifi-dialog"', page)

    def test_settings_force_confirmation_is_explicit_boolean(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 0}
        for invalid in ["yes", 1, None]:
            response = client.post("/api/settings", json={**settings, "confirm_data_loss": invalid},
                                   base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)
        response = client.post("/api/settings", json={**settings, "confirm_data_loss": True},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertTrue(monitor.last_force_save)
    def test_dashboard_port_defaults_to_5000_and_can_be_overridden(self):
        self.assertEqual(parse_args(["web"]).port, 5000)
        self.assertEqual(parse_args(["web", "--port", "5001"]).port, 5001)
        self.assertTrue(parse_args(["reboot", "--confirm"]).confirm)

    def test_home_uses_python_in_browser_title_and_wordmark(self):
        response = create_app(FakeMonitor()).test_client().get(
            "/", base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertIn(b"<title>PicoForge Dashboard | Python</title>", response.data)
        self.assertRegex(response.data, rb"<small>Python \xc2\xb7 App v[0-9][^<]*</small>")
        console = create_app(FakeMonitor()).test_client().get("/console", base_url="http://127.0.0.1")
        self.assertIn(b"<title>PicoForge Console | Python</title>", console.data)
        self.assertRegex(console.data, rb"<small>Python \xc2\xb7 App v[0-9][^<]*</small>")

    def test_status_route_returns_snapshot_and_security_headers(self):
        app = create_app(FakeMonitor())
        client = app.test_client()
        response = client.get("/api/status?board=board-a", base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["hardware"]["product_name"], "PicoForge")
        self.assertEqual(response.json["runtime"]["uptime_ms"], 42_000)
        self.assertEqual(response.json["runtime"]["cpu_usage_core0_basis_points"], 1234)
        self.assertEqual(response.json["runtime"]["cpu_usage_core1_basis_points"], 5678)
        self.assertEqual(response.json["runtime"]["ram_used_bytes"], 65536)
        self.assertEqual(response.json["runtime"]["ram_total_bytes"], 532480)
        self.assertEqual(response.json["board_status"]["temperature_centidegrees"], 2345)
        self.assertEqual(response.json["settings"], {"system_clock_mhz": 0, "sdcard_profile": 0, "radios": 0})
        self.assertIn("board_configuration", response.json)
        self.assertFalse(response.json["telemetry_stale"])
        self.assertIn("10", response.json["report_freshness"])
        self.assertEqual(response.headers["X-Frame-Options"], "DENY")
        self.assertEqual(response.headers["Cache-Control"], "no-store")

    def test_api_error_statuses_distinguish_bad_input_conflicts_and_unavailable_devices(self):
        class FailingMonitor(FakeMonitor):
            def snapshot(self, board_id=None):
                raise RuntimeError("device busy")

            def boards(self):
                raise OSError("HID enumeration failed")

        monitor = FailingMonitor()
        client = create_app(monitor).test_client()

        self.assertEqual(client.post("/api/led", json={"on": 1}).status_code, 400)
        conflict = client.get("/api/status")
        self.assertEqual(conflict.status_code, 409)
        self.assertEqual(conflict.json["error"], "device busy")
        unavailable = client.get("/api/boards")
        self.assertEqual(unavailable.status_code, 503)
        self.assertEqual(unavailable.json["error"], "HID enumeration failed")

    def test_boards_route_lists_selectable_devices(self):
        response = create_app(FakeMonitor()).test_client().get(
            "/api/boards", base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["default_board_id"], "board-a")
        self.assertEqual(response.json["boards"][0]["serial_number"], "0123456789abcdef")

    def test_serves_svg_favicon(self):
        response = create_app(FakeMonitor()).test_client().get(
            "/favicon.svg", base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.mimetype, "image/svg+xml")
        self.assertIn(b"<svg", response.data)
        response.close()

    def test_sets_led_from_boolean_json(self):
        monitor = FakeMonitor()
        response = create_app(monitor).test_client().post(
            "/api/led?board=board-a", json={"on": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["board_status"]["led_on"])
        self.assertEqual(monitor.last_board_id, "board-a")

    def test_settings_endpoint_validates_and_returns_confirmed_values(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        settings = {"system_clock_mhz": 200, "sdcard_profile": 1, "radios": 2}
        for invalid in (
            {**settings, "system_clock_mhz": 100},
            {**settings, "sdcard_profile": 2},
            {**settings, "radios": True},
            {"system_clock_mhz": 200, "sdcard_profile": 1},
        ):
            response = client.post("/api/settings", json=invalid, base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)
        response = client.post("/api/settings?board=board-a", json=settings, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["settings"], settings)
        self.assertEqual(monitor.last_settings, settings)

    def test_settings_endpoint_reports_invalid_device_readback_as_service_error(self):
        class InvalidReadbackMonitor(FakeMonitor):
            def set_settings(self, board_id, settings):
                raise ValueError("invalid HID settings report")

        response = create_app(InvalidReadbackMonitor()).test_client().post(
            "/api/settings",
            json={"system_clock_mhz": 150, "sdcard_profile": 0, "radios": 0},
            base_url="http://127.0.0.1",
        )
        self.assertEqual(response.status_code, 503)

    def test_status_snapshot_includes_board_telemetry(self):
        response = create_app(FakeMonitor()).test_client().get(
            "/api/status", base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["board_status"]["temperature_valid"])
        self.assertFalse(response.json["board_status"]["led_on"])

    def test_rejects_invalid_led_command(self):
        response = create_app(FakeMonitor()).test_client().post(
            "/api/led", json={"on": 1}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 400)

    def test_led_endpoint_reports_unavailable_device(self):
        class DisconnectedMonitor(FakeMonitor):
            def set_led(self, board_id, on):
                raise RuntimeError("board LED is unavailable")

        response = create_app(DisconnectedMonitor()).test_client().post(
            "/api/led?board=board-a", json={"on": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 409)

    def test_requests_msc_mode_with_boolean_json(self):
        response = create_app(FakeMonitor()).test_client().post(
            "/api/msc?board=board-a", json={"enabled": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["msc"]["enabled"])
        self.assertTrue(response.json["msc"]["transition_pending"])

    def test_msc_endpoint_reports_device_error(self):
        class BusyMonitor(FakeMonitor):
            def set_msc(self, board_id, enabled, *, force=False):
                raise RuntimeError("PicoForge HID is not connected")

        response = create_app(BusyMonitor()).test_client().post(
            "/api/msc?board=board-a", json={"enabled": False}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 409)
        self.assertEqual(response.json["error"], "PicoForge HID is not connected")

    def test_reboot_requires_explicit_confirmation_and_returns_status(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        response = client.post("/api/reboot", json={"confirm": 1}, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 400)
        self.assertFalse(hasattr(monitor, "last_board_id"))
        response = client.post("/api/reboot?board=board-a", json={"confirm": True},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["control_result"]["opcode"], 2)
        self.assertEqual(monitor.last_board_id, "board-a")

    def test_force_return_requires_explicit_data_loss_confirmation(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        for payload in ({}, {"confirm_data_loss": False}, {"confirm_data_loss": 1},
                        {"confirm_data_loss": "true"}):
            response = client.post("/api/msc/force-return?board=board-a", json=payload, base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)
            self.assertFalse(hasattr(monitor, "last_msc"))
        response = client.post("/api/msc/force-return?board=board-a", json={"confirm_data_loss": True},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(monitor.last_msc, (False, True))
        self.assertTrue(response.json["msc"]["transition_pending"])

    def test_self_test_endpoint_delivers_validated_async_commands(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        response = client.post("/api/self-test?board=board-a", json={"action": "start", "size_mib": 32}, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(monitor.last_self_test, ("start", 32))
        response = client.post("/api/self-test?board=board-a", json={"action": "start", "size_mib": 1024},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(monitor.last_self_test, ("start", 1024))
        response = client.post("/api/self-test?board=board-a", json={"action": "cancel", "size_mib": 32},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(monitor.last_self_test, ("cancel", None))
        for payload in ({"action": "start", "size_mib": True}, {"action": "start", "size_mib": 1025}, {"action": "invalid"}):
            response = client.post("/api/self-test?board=board-a", json=payload, base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)

    def test_self_test_endpoint_reports_device_unavailability(self):
        class DisconnectedMonitor(FakeMonitor):
            def set_self_test(self, board_id, action, size_mib):
                raise RuntimeError("PicoForge HID is not connected")
        response = create_app(DisconnectedMonitor()).test_client().post(
            "/api/self-test?board=board-a", json={"action": "cancel"}, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 409)


if __name__ == "__main__":
    unittest.main()