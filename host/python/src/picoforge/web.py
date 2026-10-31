"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import secrets
from typing import Any

from flask import Flask, jsonify, render_template, request, send_from_directory

from .monitor import DashboardBoards
from .protocol import (
    build_wireless_command_report,
    build_wireless_configuration_report,
    build_self_test_command_report,
    build_settings_command_report,
    normalize_legacy_settings,
    validate_legacy_settings,
)


def create_app(monitor: Any | None = None, web_root: Path | None = None) -> Flask:
    """Create a testable status app; hardware monitoring starts only by default."""
    installed = Path(__file__).with_name("web")  # packaged from host/web; source checkouts use host/web directly
    root = web_root or (installed if installed.is_dir() else Path(__file__).resolve().parents[3] / "web")
    dashboard = monitor or DashboardBoards()
    app = Flask(__name__, template_folder=str(root), static_folder=None)
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
        return render_template("index.html", dashboard_type="Python")

    @app.get("/favicon.svg")
    def favicon():
        return send_from_directory(root, "favicon.svg", mimetype="image/svg+xml")

    @app.get("/css/<path:filename>")
    def css(filename: str):
        return send_from_directory(root / "css", filename)

    @app.get("/js/<path:filename>")
    def javascript(filename: str):
        return send_from_directory(root / "js", filename)

    @app.get("/webhid/js/wireless-form.mjs")
    def wireless_form():
        installed_form = Path(__file__).with_name("wireless_ui")
        form_root = (installed_form if installed_form.is_dir()
                     else Path(__file__).resolve().parents[3] / "webhid" / "js")
        return send_from_directory(form_root, "wireless-form.mjs")

    @app.get("/api/status")
    def status():
        try:
            return jsonify(dashboard.snapshot(request.args.get("board")))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409

    @app.get("/api/boards")
    def boards():
        try:
            return jsonify(dashboard.boards())
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/led")
    def set_led():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("on"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'on' field"}), 400
        try:
            board_status = dashboard.set_led(request.args.get("board"), payload["on"])
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"board_status": board_status, "control_result": snapshot.get("control_result")}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.get("/api/wireless")
    def wireless():
        try:
            return jsonify(dashboard.read_wireless(request.args.get("board")))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except (OSError, ValueError):
            return jsonify({"error": "Wireless status unavailable"}), 503

    @app.post("/api/wireless/scan")
    def scan_wireless():
        try:
            return jsonify(dashboard.scan_wireless(request.args.get("board")))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except (OSError, ValueError):
            return jsonify({"error": "Wireless scan failed"}), 503

    @app.post("/api/wireless/test")
    def test_wireless():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or set(payload) - {"ssid", "password"}:
            return jsonify({"error": "Expected SSID and optional password"}), 400
        try:
            build_wireless_command_report(1, 3, ssid=payload.get("ssid", ""),
                                          password=payload.get("password", ""))
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        try:
            return jsonify(dashboard.test_wireless(request.args.get("board"), payload["ssid"],
                                                   payload.get("password", "")))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except TimeoutError:
            return jsonify({"error": "Wi-Fi test timed out; outcome is uncertain. Restart the dashboard "
                                     "and reconnect the board before retrying."}), 503
        except (OSError, ValueError):
            return jsonify({"error": "Wi-Fi test result is uncertain. Restart the dashboard "
                                     "and reconnect the board before retrying."}), 503

    @app.post("/api/wireless/cancel")
    def cancel_wireless_test():
        try:
            return jsonify(dashboard.cancel_wireless_test(request.args.get("board")))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except (OSError, ValueError):
            return jsonify({"error": "Could not clear the wireless test credentials"}), 503

    @app.post("/api/wireless")
    def set_wireless():
        payload = request.get_json(silent=True)
        try:
            build_wireless_configuration_report(payload)
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        try:
            return jsonify(dashboard.set_wireless(request.args.get("board"), payload))
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except (OSError, ValueError):
            return jsonify({"error": "Wireless provisioning outcome is uncertain; command was not resent"}), 503

    @app.post("/api/settings")
    def set_settings():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected boot settings JSON"}), 400
        board_id = request.args.get("board")
        force = payload.get("confirm_data_loss", False)
        if type(force) is not bool:
            return jsonify({"error": "confirm_data_loss must be boolean"}), 400
        if "sdcard_profile" in payload:
            settings = {key: payload.get(key) for key in ("system_clock_mhz", "sdcard_profile")}
            if "radios" in payload:
                settings["radios"] = payload["radios"]
            validator = build_settings_command_report
        else:
            settings = {key: payload.get(key) for key in
                        ("system_clock_mhz", "sdio_bus_mhz", "spi_bus_mhz")}
            validator = validate_legacy_settings
        try:
            validator(settings)
        except ValueError as error:
            return jsonify({"error": str(error)}), 400
        if "sdcard_profile" not in settings:
            try:
                snapshot = dashboard.snapshot(board_id)
                hardware = snapshot.get("hardware")
                configuration = snapshot.get("board_configuration")
                if not isinstance(hardware, dict):
                    raise RuntimeError("Waiting for device identity")
                current_profile = (
                    configuration.get("sdcard_profile") if isinstance(configuration, dict) else None
                )
                settings = normalize_legacy_settings(
                    settings, hardware["storage_transport"], current_profile
                )
            except ValueError as error:
                return jsonify({"error": str(error)}), 400
            except RuntimeError as error:
                return jsonify({"error": str(error)}), 409
        try:
            saved = (dashboard.set_settings(board_id, settings, force=True) if force
                     else dashboard.set_settings(board_id, settings))
            snapshot = dashboard.snapshot(board_id)
            return jsonify({"settings": saved, "control_result": snapshot.get("control_result")}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except (OSError, ValueError) as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/msc")
    def set_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("enabled"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'enabled' field"}), 400
        try:
            msc = dashboard.set_msc(request.args.get("board"), payload["enabled"])
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"msc": msc, "control_result": snapshot.get("control_result")}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    @app.post("/api/reboot")
    def reboot():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm") is not True:
            return jsonify({"error": "Explicit confirm: true is required"}), 400
        try:
            return jsonify({"control_result": dashboard.reboot(request.args.get("board"))}), 200
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
        size_mib = payload.get("size_mib")
        command_size = None if action == "cancel" else size_mib
        try:
            build_self_test_command_report(action, command_size)
            dashboard.set_self_test(request.args.get("board"), action, command_size)
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"accepted": True, "control_result": snapshot.get("control_result")}), 202
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
            msc = dashboard.set_msc(request.args.get("board"), False, force=True)
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"msc": msc, "control_result": snapshot.get("control_result")}), 200
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            return jsonify({"error": str(error)}), 503

    app.extensions["picoforge_monitor"] = dashboard
    return app


def run_server(port: int = 5000, serial_number: str | None = None, device_path: str | None = None) -> None:
    dashboard = DashboardBoards(serial_number, device_path)
    try:
        create_app(dashboard).run(host="127.0.0.1", port=port, debug=False, use_reloader=False)
    finally:
        dashboard.close()