import unittest

from picoforge.web import create_app


class FakeMonitor:
    def snapshot(self):
        return {
            "connected": True,
            "error": None,
            "hardware": {
                "product_name": "PicoForge",
                "mcu": "RP2040",
                "mcu_id": 1,
                "serial_number": "0123456789abcdef",
            },
            "runtime": {
                "version": 3,
                "sequence": 7,
                "uptime_ms": 42_000,
                "system_clock_hz": 125_000_000,
                "usb_mounted": True,
                "cdc_open": False,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": False,
                "cpu_usage_basis_points": 1234,
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
            "updated_at": "2026-10-10T12:00:00+00:00",
        }

    def set_led(self, on):
        return {"temperature_valid": True, "temperature_centidegrees": 2345,
                "led_available": True, "led_on": on}

    def set_msc(self, enabled, *, force=False):
        self.last_msc = (enabled, force)
        return {"enabled": enabled, "transition_pending": True, "request_rejected": False}

    def set_self_test(self, action, size_mib):
        self.last_self_test = (action, size_mib)


class WebTests(unittest.TestCase):
    def test_status_route_returns_snapshot_and_security_headers(self):
        app = create_app(FakeMonitor())
        client = app.test_client()
        response = client.get("/api/status", base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["hardware"]["product_name"], "PicoForge")
        self.assertEqual(response.json["runtime"]["uptime_ms"], 42_000)
        self.assertEqual(response.json["runtime"]["cpu_usage_basis_points"], 1234)
        self.assertEqual(response.json["runtime"]["ram_used_bytes"], 65536)
        self.assertEqual(response.json["runtime"]["ram_total_bytes"], 532480)
        self.assertEqual(response.json["board_status"]["temperature_centidegrees"], 2345)
        self.assertEqual(response.headers["X-Frame-Options"], "DENY")
        self.assertEqual(response.headers["Cache-Control"], "no-store")

    def test_serves_svg_favicon(self):
        response = create_app(FakeMonitor()).test_client().get(
            "/favicon.svg", base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.mimetype, "image/svg+xml")
        self.assertIn(b"<svg", response.data)
        response.close()

    def test_sets_led_from_boolean_json(self):
        response = create_app(FakeMonitor()).test_client().post(
            "/api/led", json={"on": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["board_status"]["led_on"])

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
            def set_led(self, on):
                raise RuntimeError("board LED is unavailable")

        response = create_app(DisconnectedMonitor()).test_client().post(
            "/api/led", json={"on": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 409)

    def test_requests_msc_mode_with_boolean_json(self):
        response = create_app(FakeMonitor()).test_client().post(
            "/api/msc", json={"enabled": True}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["msc"]["enabled"])
        self.assertTrue(response.json["msc"]["transition_pending"])

    def test_msc_endpoint_reports_device_error(self):
        class BusyMonitor(FakeMonitor):
            def set_msc(self, enabled):
                raise RuntimeError("PicoForge HID is not connected")

        response = create_app(BusyMonitor()).test_client().post(
            "/api/msc", json={"enabled": False}, base_url="http://127.0.0.1"
        )
        self.assertEqual(response.status_code, 409)
        self.assertEqual(response.json["error"], "PicoForge HID is not connected")

    def test_force_return_requires_explicit_data_loss_confirmation(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        for payload in ({}, {"confirm_data_loss": False}, {"confirm_data_loss": 1},
                        {"confirm_data_loss": "true"}):
            response = client.post("/api/msc/force-return", json=payload, base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)
            self.assertFalse(hasattr(monitor, "last_msc"))
        response = client.post("/api/msc/force-return", json={"confirm_data_loss": True},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(monitor.last_msc, (False, True))
        self.assertTrue(response.json["msc"]["transition_pending"])

    def test_self_test_endpoint_delivers_validated_async_commands(self):
        monitor = FakeMonitor()
        client = create_app(monitor).test_client()
        response = client.post("/api/self-test", json={"action": "start", "size_mib": 32}, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(monitor.last_self_test, ("start", 32))
        response = client.post("/api/self-test", json={"action": "start", "size_mib": 1024},
                               base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 202)
        self.assertEqual(monitor.last_self_test, ("start", 1024))
        for payload in ({"action": "start", "size_mib": True}, {"action": "start", "size_mib": 1025}, {"action": "invalid"}):
            response = client.post("/api/self-test", json=payload, base_url="http://127.0.0.1")
            self.assertEqual(response.status_code, 400)

    def test_self_test_endpoint_reports_device_unavailability(self):
        class DisconnectedMonitor(FakeMonitor):
            def set_self_test(self, action, size_mib):
                raise RuntimeError("PicoForge HID is not connected")
        response = create_app(DisconnectedMonitor()).test_client().post(
            "/api/self-test", json={"action": "cancel"}, base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 409)


if __name__ == "__main__":
    unittest.main()