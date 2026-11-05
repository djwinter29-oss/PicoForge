"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import base64
import binascii
import secrets
from typing import cast

from flask import Flask, jsonify, render_template, request, send_from_directory

from .boards import DashboardBoards
from .console import ConsoleBridge
from .monitor import _empty_snapshot
from .. import __version__
from ..hid.device import WirelessTestCancelled
from ..hid.models import WirelessStatus
from ..reports import InvalidRequest, ProtocolError
from ..reports.wireless import request_ssid, validate_wireless_configuration

_WIRELESS_RETRY = "Restart the dashboard and reconnect the board before retrying."
_INTERNAL_WIRELESS_FIELDS = frozenset({"wifi_state_id", "tcp_cli_auth_required", "age_ms", "error"})


def _public_wireless(status: WirelessStatus) -> WirelessStatus:
    return cast(WirelessStatus, {
        key: value for key, value in status.items() if key not in _INTERNAL_WIRELESS_FIELDS
    })


def _unavailable(message: str) -> tuple:
    return jsonify({"error": message}), 503


def _limit_console_request() -> None:
    request.max_content_length = 2048


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
        if type(error) is RuntimeError or isinstance(error, WirelessTestCancelled):
            return jsonify({"error": str(error)}), 409
        app.logger.exception("Unhandled RuntimeError")
        return jsonify({"error": "Internal server error"}), 500

    @app.errorhandler(LookupError)
    def not_found(error: LookupError):
        if type(error) is not LookupError:
            raise error  # KeyError/IndexError are bugs, not unknown boards; let Flask report 500.
        return jsonify({"error": str(error)}), 404

    @app.errorhandler(ValueError)
    def invalid_request_or_bug(error: ValueError):
        if isinstance(error, InvalidRequest):
            return jsonify({"error": str(error)}), 400
        app.logger.exception("Unhandled ValueError")
        return jsonify({"error": "Internal server error"}), 500

    @app.errorhandler(ProtocolError)
    def protocol_error(error: ProtocolError):
        return jsonify({"error": str(error)}), 503

    @app.errorhandler(OSError)
    def unavailable(error: OSError):
        return jsonify({"error": str(error)}), 503

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
    @app.post("/api/console")
    def console_connect():
        _limit_console_request()
        try:
            payload = request.get_json(silent=True)
            if not isinstance(payload, dict) or payload:
                return jsonify({
                    "error": "Expected an empty object; TCP authentication uses the connected board"
                }), 400
            board_id = request.args.get("board")
            monitor = boards.monitor(board_id)
            wireless = monitor.snapshot().get("wireless")
            if wireless is None:
                raise RuntimeError("Waiting for the board's wireless status")
            return jsonify({
                "session": console.open(wireless, monitor.tcp_cli_auth_proof, board_id=board_id),
            })
        except OSError as error:
            app.logger.warning("TCP console connection failed: %s", error)
            return jsonify({"error": f"TCP console connection failed: {error}"}), 503

    @app.get("/api/console")
    def console_read():
        _limit_console_request()
        data, closed = console.read(request.args.get("session"))
        return jsonify({"data": base64.b64encode(data).decode("ascii"), "closed": closed})

    @app.put("/api/console")
    def console_write():
        _limit_console_request()
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
        console.write(request.args.get("session"), data)
        return jsonify({"ok": True})

    @app.delete("/api/console")
    def console_close():
        _limit_console_request()
        console.close(request.args.get("session"))
        return jsonify({"ok": True})


def _register_api_routes(app: Flask, boards: DashboardBoards) -> None:
    @app.get("/api/status")
    def status():
        board_id = request.args.get("board")
        if board_id is None:
            return jsonify(_empty_snapshot())
        snapshot = boards.monitor(board_id).status_snapshot()
        wireless = snapshot.get("wireless")
        if wireless is not None:
            snapshot["wireless"] = _public_wireless(wireless)
        return jsonify(snapshot)

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
        if action not in ("start", "cancel"):
            return jsonify({"error": "Self-test action must be start or cancel"}), 400
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
        try:
            return jsonify(_board(boards).scan_wireless())
        except (OSError, ProtocolError):
            app.logger.exception("Wireless scan failed")
            return _unavailable("Wireless scan failed")

    @app.post("/api/wireless/test")
    def test_wireless():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or set(payload) - {"ssid", "ssid_hex", "password"}:
            return jsonify({"error": "Expected SSID and optional password"}), 400
        ssid = request_ssid(payload) if "ssid_hex" in payload else payload.get("ssid", "")
        try:
            return jsonify(_board(boards).test_wireless(ssid, payload.get("password", "")))
        except TimeoutError:
            app.logger.exception("Wi-Fi test timed out")
            return _unavailable(f"Wi-Fi test timed out; outcome is uncertain. {_WIRELESS_RETRY}")
        except (OSError, ProtocolError):
            app.logger.exception("Wi-Fi test result is uncertain")
            return _unavailable(f"Wi-Fi test result is uncertain. {_WIRELESS_RETRY}")

    @app.post("/api/wireless/cancel")
    def cancel_wireless_test():
        try:
            return jsonify(_board(boards).cancel_wireless_test())
        except (OSError, ProtocolError):
            app.logger.exception("Could not clear wireless test credentials")
            return _unavailable("Could not clear the wireless test credentials")

    @app.post("/api/wireless")
    def set_wireless():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected wireless configuration JSON"}), 400
        validate_wireless_configuration(payload)  # Bad input is 400 whatever the board state.
        try:
            result = _board(boards).set_wireless(payload)
            result["wireless"] = _public_wireless(result["wireless"])
            return jsonify(result)
        except (OSError, ProtocolError):
            app.logger.exception("Wireless provisioning outcome is uncertain")
            return _unavailable("Wireless provisioning outcome is uncertain; command was not resent")


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