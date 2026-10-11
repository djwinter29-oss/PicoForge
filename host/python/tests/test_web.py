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
                "version": 2,
                "sequence": 7,
                "uptime_ms": 42_000,
                "system_clock_hz": 125_000_000,
                "usb_mounted": True,
                "cdc_open": False,
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": False,
            },
            "board_status": {
                "temperature_valid": True,
                "temperature_centidegrees": 2345,
                "led_available": True,
                "led_on": False,
            },
            "updated_at": "2026-10-10T12:00:00+00:00",
        }

    def set_led(self, on):
        return {"temperature_valid": True, "temperature_centidegrees": 2345,
                "led_available": True, "led_on": on}


class WebTests(unittest.TestCase):
    def test_status_route_returns_snapshot_and_security_headers(self):
        app = create_app(FakeMonitor())
        client = app.test_client()
        response = client.get("/api/status", base_url="http://127.0.0.1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["hardware"]["product_name"], "PicoForge")
        self.assertEqual(response.json["runtime"]["uptime_ms"], 42_000)
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


if __name__ == "__main__":
    unittest.main()