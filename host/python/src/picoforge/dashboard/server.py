"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import base64
import binascii
import secrets

from flask import Flask, jsonify, render_template, request, send_from_directory

from .boards import DashboardBoards
from .console import ConsoleBridge
from .monitor import _empty_snapshot
from .. import __version__
from ..reports import InvalidRequest
from ..reports.wireless import request_ssid, validate_wireless_configuration

_WIRELESS_RETRY = "Restart the dashboard and reconnect the board before retrying."
# Endpoint -> message replacing device or transport detail when a wireless command fails with 503.
_UNAVAILABLE_MESSAGES = {
    "scan_wireless": "Wireless scan failed",
    "test_wireless": f"Wi-Fi test result is uncertain. {_WIRELESS_RETRY}",
    "cancel_wireless_test": "Could not clear the wireless test credentials",
    "set_wireless": "Wireless provisioning outcome is uncertain; command was not resent",
}


def create_app(dashboard: DashboardBoards | None = None, web_root: Path | None = None) -> Flask:
    """Create a testable status app; hardware monitoring starts only by default."""
    installed = Path(__file__).resolve().parents[1] / "web"
    root = web_root or (installed if installed.is_dir() else Path(__file__).resolve().parents[4] / "web")
    boards = dashboard if dashboard is not None else DashboardBoards()
    console = ConsoleBridge()
    app = Flask(__name__, template_folder=str(root), static_folder=None)
    app.config.update(
        SECRET_KEY=secrets.token_bytes(32),
        TRUSTED_HOSTS=["localhost", "127.0.0.1"],
    )

    @app.before_request
    def same_origin_writes():
        origin = request.headers.get("Origin")
        if (request.method != "GET" and request.path.startswith("/api/")
                and origin is not None and origin != request.host_url.rstrip("/")):
            return jsonify({"error": "API writes must be same-origin"}), 403
        return None

    @app.errorhandler(RuntimeError)
    def conflict(error: RuntimeError):
        return jsonify({"error": str(error)}), 409

    @app.errorhandler(LookupError)
    def not_found(error: LookupError):
        if type(error) is not LookupError:
            raise error  # KeyError/IndexError are bugs, not unknown boards; let Flask report 500.
        return jsonify({"error": str(error)}), 404

    @app.errorhandler(ValueError)
    def invalid_or_unavailable(error: ValueError):
        if isinstance(error, InvalidRequest):
            return jsonify({"error": str(error)}), 400
        # ponytail: any other ValueError is treated as malformed board data, so a code bug raising ValueError
        # also reads as 503. Raise a dedicated protocol-error subclass from the parsers if that hides a real bug.
        return unavailable(error)

    @app.errorhandler(OSError)
    def unavailable(error: Exception):
        message = _UNAVAILABLE_MESSAGES.get(request.endpoint or "", str(error))
        if request.endpoint == "test_wireless" and isinstance(error, TimeoutError):
            message = f"Wi-Fi test timed out; outcome is uncertain. {_WIRELESS_RETRY}"
        return jsonify({"error": message}), 503

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
    _register_console_routes(app, boards, console)
    _register_api_routes(app, boards)
    _register_wireless_routes(app, boards)
    app.extensions["picoforge_monitor"] = boards
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


def _board(boards: DashboardBoards):
    return boards.monitor(request.args.get("board"))


def _register_console_routes(app: Flask, boards: DashboardBoards, console: ConsoleBridge) -> None:
    @app.route("/api/console", methods=["POST", "GET", "PUT", "DELETE"])
    def console_api():
        request.max_content_length = 2048
        try:
            if request.method == "POST":
                payload = request.get_json(silent=True)
                if not isinstance(payload, dict) or payload:
                    return jsonify({
                        "error": "Expected an empty object; TCP authentication uses the connected board"
                    }), 400
                monitor = _board(boards)
                return jsonify({"session": console.open(monitor.read_wireless(), monitor.tcp_cli_auth_proof)})
            session = request.args.get("session")
            if request.method == "GET":
                data, closed = console.read(session)
                return jsonify({"data": base64.b64encode(data).decode("ascii"), "closed": closed})
            if request.method == "PUT":
                payload = request.get_json(silent=True)
                if (not isinstance(payload, dict) or set(payload) != {"data"}
                        or not isinstance(payload["data"], str) or len(payload["data"]) > 1368):
                    return jsonify({"error": "Expected up to 1024 base64-encoded input bytes"}), 400
                try:
                    data = base64.b64decode(payload["data"], validate=True)
                except binascii.Error:
                    data = b""
                if not 0 < len(data) <= 1024:
                    return jsonify({"error": "Expected 1..1024 input bytes"}), 400
                console.write(session, data)
            else:
                console.close(session)
            return jsonify({"ok": True})
        except OSError as error:
            app.logger.warning("TCP console connection failed: %s", error)
            return jsonify({"error": f"TCP console connection failed: {error}"}), 503


def _register_api_routes(app: Flask, boards: DashboardBoards) -> None:
    @app.get("/api/status")
    def status():
        board_id = request.args.get("board")
        return jsonify(_empty_snapshot() if board_id is None else boards.monitor(board_id).snapshot())

    @app.get("/api/boards")
    def boards_list():
        return jsonify(boards.boards())

    @app.post("/api/led")
    def set_led():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("on"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'on' field"}), 400
        return jsonify(_board(boards).set_led(payload["on"]))

    @app.post("/api/settings")
    def set_settings():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected boot settings JSON"}), 400
        force = payload.pop("confirm_data_loss", False)
        if type(force) is not bool:
            return jsonify({"error": "confirm_data_loss must be boolean"}), 400
        return jsonify(_board(boards).set_settings(payload, force=force))

    @app.post("/api/msc")
    def set_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("enabled"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'enabled' field"}), 400
        return jsonify(_board(boards).set_msc(payload["enabled"]))

    @app.post("/api/reboot")
    def reboot():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm") is not True:
            return jsonify({"error": "Explicit confirm: true is required"}), 400
        return jsonify(_board(boards).reboot())

    @app.post("/api/self-test")
    def self_test():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected a self-test JSON command"}), 400
        action = payload.get("action")
        size_mib = None if action == "cancel" else payload.get("size_mib")
        return jsonify(_board(boards).set_self_test(action, size_mib)), 202

    @app.post("/api/msc/force-return")
    def force_return_msc():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm_data_loss") is not True:
            return jsonify({"error": "Explicit confirm_data_loss: true is required"}), 400
        return jsonify(_board(boards).set_msc(False, force=True))


def _register_wireless_routes(app: Flask, boards: DashboardBoards) -> None:
    @app.post("/api/wireless/scan")
    def scan_wireless():
        return jsonify(_board(boards).scan_wireless())

    @app.post("/api/wireless/test")
    def test_wireless():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or set(payload) - {"ssid", "ssid_hex", "password"}:
            return jsonify({"error": "Expected SSID and optional password"}), 400
        ssid = request_ssid(payload) if "ssid_hex" in payload else payload.get("ssid", "")
        return jsonify(_board(boards).test_wireless(ssid, payload.get("password", "")))

    @app.post("/api/wireless/cancel")
    def cancel_wireless_test():
        return jsonify(_board(boards).cancel_wireless_test())

    @app.post("/api/wireless")
    def set_wireless():
        payload = request.get_json(silent=True)
        validate_wireless_configuration(payload)  # Bad input is 400 whatever the board state.
        return jsonify(_board(boards).set_wireless(payload))


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