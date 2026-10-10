"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import secrets
from typing import Any

from flask import Flask, jsonify, send_from_directory

from .monitor import DeviceMonitor


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

    @app.get("/css/<path:filename>")
    def css(filename: str):
        return send_from_directory(root / "css", filename)

    @app.get("/js/<path:filename>")
    def javascript(filename: str):
        return send_from_directory(root / "js", filename)

    @app.get("/api/status")
    def status():
        return jsonify(dashboard.snapshot())

    app.extensions["picoforge_monitor"] = dashboard
    return app


def run_server(port: int = 8765, serial_number: str | None = None, device_path: str | None = None) -> None:
    monitor = DeviceMonitor(serial_number, device_path)
    try:
        create_app(monitor).run(host="127.0.0.1", port=port, debug=False, use_reloader=False)
    finally:
        monitor.close()