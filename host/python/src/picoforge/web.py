"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import secrets
from typing import Any

from flask import Flask, jsonify, request, send_from_directory

from .monitor import DeviceMonitor
from .protocol import build_self_test_command_report


def create_app(monitor: Any | None = None, web_root: Path | None = None) -> Flask:
    """Create a testable status app; hardware monitoring starts only by default."""
    root = web_root or Path(__file__).resolve().parents[3] / "web"
    dashboard = monitor or DeviceMonitor()
    app = Flask(__name__, static_folder=None)
    app.config.update(
        SECRET_KEY=secrets.token_bytes(32),
        TRUSTED_HOSTS=["localhost", "127.0.0.1"],
    )

    @app.after_request
    def security_headers(response):
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Cache-Control"] = "no-store"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; connect-src 'self'; style-src 'self'; script-src 'self'; "
            "img-src 'self'; frame-ancestors 'none'"
        )
        return response

    @app.get("/")
    def index():
        return send_from_directory(root, "index.html")

    @app.get("/favicon.svg")
    def favicon():
        return send_from_directory(root, "favicon.svg", mimetype="image/svg+xml")

    @app.get("/css/<path:filename>")
    def css(filename: str):
        return send_from_directory(root / "css", filename)

    @app.get("/js/<path:filename>")
    def javascript(filename: str):
        return send_from_directory(root / "js", filename)

    @app.get("/api/status")
    def status():
        return jsonify(dashboard.snapshot())

    @app.post("/api/led")
    def set_led():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("on"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'on' field"}), 400
        try:
            return jsonify({"board_status": dashboard.set_led(payload["on"])}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/msc")
    def set_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("enabled"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'enabled' field"}), 400
        try:
            return jsonify({"msc": dashboard.set_msc(payload["enabled"])}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/self-test")
    def self_test():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected a self-test JSON command"}), 400
        action = payload.get("action")
        size_mib = payload.get("size_mib", 32)
        try:
            build_self_test_command_report(action, size_mib)
            dashboard.set_self_test(action, size_mib)
            return jsonify({"accepted": True}), 202
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/msc/force-return")
    def force_return_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm_data_loss") is not True:
            return jsonify({"error": "Explicit confirm_data_loss: true is required"}), 400
        try:
            return jsonify({"msc": dashboard.set_msc(False, force=True)}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    app.extensions["picoforge_monitor"] = dashboard
    return app


def run_server(port: int = 8765, serial_number: str | None = None, device_path: str | None = None) -> None:
    monitor = DeviceMonitor(serial_number, device_path)
    try:
        create_app(monitor).run(host="127.0.0.1", port=port, debug=False, use_reloader=False)
    finally:
        monitor.close()