"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import base64
import binascii
import secrets

from flask import Flask, jsonify, render_template, request, send_from_directory

from .backend import DashboardBackend
from .boards import DashboardBoards
from .console import ConsoleBridge
from .. import __version__
from ..reports import (
    build_wireless_command_report,
    build_self_test_command_report,
    build_settings_command_report,
    validate_wireless_configuration,
)


def _api_error(
    error: RuntimeError | OSError | ValueError,
    *,
    value_error_status: int = 400,
    unavailable_message: str | None = None,
):
    if isinstance(error, RuntimeError):
        status = 409
    elif isinstance(error, OSError):
        status = 503
    else:
        status = value_error_status
    message = unavailable_message if status == 503 and unavailable_message else str(error)
    return jsonify({"error": message}), status


def create_app(monitor: DashboardBackend | None = None, web_root: Path | None = None) -> Flask:
    """Create a testable status app; hardware monitoring starts only by default."""
    installed = Path(__file__).resolve().parents[1] / "web"
    root = web_root or (installed if installed.is_dir() else Path(__file__).resolve().parents[4] / "web")
    dashboard: DashboardBackend = monitor if monitor is not None else DashboardBoards()
    console = ConsoleBridge()
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

    _register_ui_routes(app, root)
    _register_console_routes(app, dashboard, console)
    _register_api_routes(app, dashboard)
    _register_wireless_routes(app, dashboard)
    app.extensions["picoforge_monitor"] = dashboard
    app.extensions["picoforge_console"] = console
    return app


def _register_ui_routes(app: Flask, root: Path) -> None:
    @app.get("/")
    def index():
        return render_template("index.html", dashboard_type="Python", app_version=__version__)

    @app.get("/console")
    def console_page():
        return render_template("console.html", dashboard_type="Python", app_version=__version__)

    @app.get("/favicon.svg")
    def favicon():
        return send_from_directory(root, "favicon.svg", mimetype="image/svg+xml")

    @app.get("/css/<path:filename>")
    def css(filename: str):
        return send_from_directory(root / "css", filename)

    @app.get("/js/<path:filename>")
    def javascript(filename: str):
        return send_from_directory(root / "js", filename)


def _register_console_routes(app: Flask, dashboard: DashboardBackend, console: ConsoleBridge) -> None:
    @app.route("/api/console", methods=["POST", "GET", "PUT", "DELETE"])
    def console_api():
        request.max_content_length = 2048
        origin = request.headers.get("Origin")
        if origin is not None and origin != request.host_url.rstrip("/"):
            return jsonify({"error": "Console requests must be same-origin"}), 403
        try:
            if request.method == "POST":
                payload = request.get_json(silent=True)
                if not isinstance(payload, dict) or payload:
                    return jsonify({
                        "error": "Expected an empty object; TCP authentication uses the connected board"
                    }), 400
                board_id = request.args.get("board")
                status = dashboard.read_wireless(board_id)["wireless"]
                sign_proof = lambda nonce, uid: dashboard.tcp_cli_auth_proof(board_id, nonce, uid)
                return jsonify({"session": console.open(status, sign_proof)})
            session = request.args.get("session")
            if request.method == "GET":
                data, closed = console.read(session)
                return jsonify({"data": base64.b64encode(data).decode("ascii"), "closed": closed})
            if request.method == "PUT":
                payload = request.get_json(silent=True)
                if (not isinstance(payload, dict) or set(payload) != {"data"}
                        or not isinstance(payload["data"], str) or len(payload["data"]) > 1368):
                    return jsonify({"error": "Expected up to 1024 base64-encoded input bytes"}), 400
                data = base64.b64decode(payload["data"], validate=True)
                if not 0 < len(data) <= 1024:
                    return jsonify({"error": "Expected 1..1024 input bytes"}), 400
                console.write(session, data)
            else:
                console.close(session)
            return jsonify({"ok": True})
        except (ValueError, binascii.Error) as error:
            return jsonify({"error": str(error)}), 400
        except RuntimeError as error:
            return jsonify({"error": str(error)}), 409
        except OSError as error:
            app.logger.warning("TCP console connection failed: %s", error)
            return jsonify({"error": f"TCP console connection failed: {error}"}), 503


def _register_api_routes(app: Flask, dashboard: DashboardBackend) -> None:
    @app.get("/api/status")
    def status():
        try:
            return jsonify(dashboard.snapshot(request.args.get("board")))
        except RuntimeError as error:
            return _api_error(error)

    @app.get("/api/boards")
    def boards():
        try:
            return jsonify(dashboard.boards())
        except (RuntimeError, OSError) as error:
            return _api_error(error)

    @app.post("/api/led")
    def set_led():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("on"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'on' field"}), 400
        try:
            board_status = dashboard.set_led(request.args.get("board"), payload["on"])
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"board_status": board_status, "control_result": snapshot.get("control_result")}), 200
        except (RuntimeError, OSError) as error:
            return _api_error(error)

    @app.post("/api/settings")
    def set_settings():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected boot settings JSON"}), 400
        board_id = request.args.get("board")
        force = payload.get("confirm_data_loss", False)
        if type(force) is not bool:
            return jsonify({"error": "confirm_data_loss must be boolean"}), 400
        settings = {key: payload.get(key) for key in ("system_clock_mhz", "sdcard_profile", "radios")}
        try:
            build_settings_command_report(settings)
        except ValueError as error:
            return _api_error(error)
        try:
            saved = (dashboard.set_settings(board_id, settings, force=True) if force
                     else dashboard.set_settings(board_id, settings))
            snapshot = dashboard.snapshot(board_id)
            return jsonify({"settings": saved, "control_result": snapshot.get("control_result")}), 200
        except (RuntimeError, OSError, ValueError) as error:
            return _api_error(error, value_error_status=503)

    @app.post("/api/msc")
    def set_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("enabled"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'enabled' field"}), 400
        try:
            msc = dashboard.set_msc(request.args.get("board"), payload["enabled"])
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"msc": msc, "control_result": snapshot.get("control_result")}), 200
        except (RuntimeError, OSError) as error:
            return _api_error(error)

    @app.post("/api/reboot")
    def reboot():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm") is not True:
            return jsonify({"error": "Explicit confirm: true is required"}), 400
        try:
            return jsonify({"control_result": dashboard.reboot(request.args.get("board"))}), 200
        except (RuntimeError, OSError) as error:
            return _api_error(error)

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
            return _api_error(error)
        except (RuntimeError, OSError) as error:
            return _api_error(error)

    @app.post("/api/msc/force-return")
    def force_return_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm_data_loss") is not True:
            return jsonify({"error": "Explicit confirm_data_loss: true is required"}), 400
        try:
            msc = dashboard.set_msc(request.args.get("board"), False, force=True)
            snapshot = dashboard.snapshot(request.args.get("board"))
            return jsonify({"msc": msc, "control_result": snapshot.get("control_result")}), 200
        except (RuntimeError, OSError) as error:
            return _api_error(error)


def _register_wireless_routes(app: Flask, dashboard: DashboardBackend) -> None:
    @app.get("/api/wireless")
    def wireless():
        try:
            return jsonify(dashboard.read_wireless(request.args.get("board")))
        except RuntimeError as error:
            return _api_error(error)
        except (OSError, ValueError) as error:
            return _api_error(error, value_error_status=503, unavailable_message="Wireless status unavailable")

    @app.post("/api/wireless/scan")
    def scan_wireless():
        try:
            return jsonify(dashboard.scan_wireless(request.args.get("board")))
        except RuntimeError as error:
            return _api_error(error)
        except (OSError, ValueError) as error:
            return _api_error(error, value_error_status=503, unavailable_message="Wireless scan failed")

    @app.post("/api/wireless/test")
    def test_wireless():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or set(payload) - {"ssid", "password"}:
            return jsonify({"error": "Expected SSID and optional password"}), 400
        try:
            build_wireless_command_report(1, 3, ssid=payload.get("ssid", ""),
                                          password=payload.get("password", ""))
        except ValueError as error:
            return _api_error(error)
        try:
            return jsonify(dashboard.test_wireless(request.args.get("board"), payload["ssid"],
                                                   payload.get("password", "")))
        except RuntimeError as error:
            return _api_error(error)
        except TimeoutError as error:
            return _api_error(
                error,
                unavailable_message="Wi-Fi test timed out; outcome is uncertain. Restart the dashboard "
                                    "and reconnect the board before retrying.",
            )
        except (OSError, ValueError) as error:
            return _api_error(
                error,
                value_error_status=503,
                unavailable_message="Wi-Fi test result is uncertain. Restart the dashboard "
                                    "and reconnect the board before retrying.",
            )

    @app.post("/api/wireless/cancel")
    def cancel_wireless_test():
        try:
            return jsonify(dashboard.cancel_wireless_test(request.args.get("board")))
        except RuntimeError as error:
            return _api_error(error)
        except (OSError, ValueError) as error:
            return _api_error(
                error,
                value_error_status=503,
                unavailable_message="Could not clear the wireless test credentials",
            )

    @app.post("/api/wireless")
    def set_wireless():
        payload = request.get_json(silent=True)
        try:
            validate_wireless_configuration(payload)
        except ValueError as error:
            return _api_error(error)
        try:
            return jsonify(dashboard.set_wireless(request.args.get("board"), payload))
        except RuntimeError as error:
            return _api_error(error)
        except (OSError, ValueError) as error:
            return _api_error(
                error,
                value_error_status=503,
                unavailable_message="Wireless provisioning outcome is uncertain; command was not resent",
            )


def run_server(port: int = 5000, serial_number: str | None = None, device_path: str | None = None) -> None:
    dashboard = DashboardBoards(serial_number, device_path)
    app: Flask | None = None
    try:
        app = create_app(dashboard)
        app.run(host="127.0.0.1", port=port, debug=False, use_reloader=False)
    finally:
        try:
            if app is not None:
                app.extensions["picoforge_console"].close_all()
        finally:
            dashboard.close()