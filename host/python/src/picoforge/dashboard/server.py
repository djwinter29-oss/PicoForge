# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Loopback-only Flask status dashboard."""

from __future__ import annotations

from pathlib import Path
import base64
import binascii
import secrets

from flask import Flask, Response, jsonify, render_template, request, send_from_directory
from werkzeug.exceptions import RequestEntityTooLarge

from .boards import DashboardBoards
from .console import ConsoleBridge
from .monitor import BoardConflict, CommandOutcomeUncertain, DeviceMonitor, _empty_snapshot
from .. import __version__
from ..hid.device import WirelessTestCancelled
from ..reports.models import WirelessStatus
from ..reports import (
    ControlOpcode, InvalidRequest, ProtocolError, build_self_test_command_report, build_wireless_command_report,
)
from ..reports.common import _validate_settings
from ..reports.wireless import request_ssid, validate_wireless_configuration

_INTERNAL_WIRELESS_FIELDS = frozenset({"wifi_state_id", "tcp_cli_auth_required", "age_ms", "error"})


def _public_wireless(status: WirelessStatus) -> dict[str, object]:
    """Serialize the HTTP shape without pretending it still contains every internal HID field."""
    return {
        key: value for key, value in status.items() if key not in _INTERNAL_WIRELESS_FIELDS
    }


def _unavailable(message: str, *, uncertain: bool = False) -> tuple[Response, int]:
    return jsonify({"error": message, "uncertain": uncertain}), 503


def create_app(dashboard: DashboardBoards | None = None, web_root: Path | None = None) -> Flask:
    """Create a testable status app; hardware monitoring starts only by default."""
    installed = Path(__file__).resolve().parents[1] / "web"
    root = web_root or (installed if installed.is_dir() else Path(__file__).resolve().parents[4] / "web/src")
    boards = dashboard if dashboard is not None else DashboardBoards()
    console = ConsoleBridge()
    app = Flask(__name__, template_folder=str(root), static_folder=None)
    app.config.update(
        SECRET_KEY=secrets.token_bytes(32),
        TRUSTED_HOSTS=["localhost", "127.0.0.1"],
        MAX_CONTENT_LENGTH=4096,
    )

    @app.before_request
    def same_origin_writes() -> tuple[Response, int] | None:
        if request.path == "/api/console":
            request.max_content_length = 2048
        limit = request.max_content_length
        if limit is not None and request.content_length is not None and request.content_length > limit:
            raise RequestEntityTooLarge()
        origin = request.headers.get("Origin")
        if (request.method != "GET" and request.path.startswith("/api/")
                and origin is not None and origin != request.host_url.rstrip("/")):
            return jsonify({"error": "API writes must be same-origin"}), 403
        return None

    @app.errorhandler(RequestEntityTooLarge)
    def request_too_large(_error: RequestEntityTooLarge) -> tuple[Response, int]:
        return jsonify({"error": "Request body exceeds the API size limit"}), 413

    @app.errorhandler(RuntimeError)
    def conflict(error: RuntimeError) -> tuple[Response, int]:
        if isinstance(error, (BoardConflict, WirelessTestCancelled)):
            return jsonify({"error": str(error)}), 409
        app.logger.exception("Unhandled RuntimeError")
        return jsonify({"error": "Internal server error"}), 500

    @app.errorhandler(CommandOutcomeUncertain)
    def uncertain_command(error: CommandOutcomeUncertain) -> tuple[Response, int]:
        return jsonify({"error": str(error), "uncertain": True}), 503

    @app.errorhandler(LookupError)
    def not_found(error: LookupError) -> tuple[Response, int]:
        if type(error) is not LookupError:
            raise error  # KeyError/IndexError are bugs, not unknown boards; let Flask report 500.
        return jsonify({"error": str(error)}), 404

    @app.errorhandler(ValueError)
    def invalid_request_or_bug(error: ValueError) -> tuple[Response, int]:
        if isinstance(error, InvalidRequest):
            return jsonify({"error": str(error)}), 400
        app.logger.exception("Unhandled ValueError")
        return jsonify({"error": "Internal server error"}), 500

    @app.errorhandler(ProtocolError)
    def protocol_error(error: ProtocolError) -> tuple[Response, int]:
        return jsonify({"error": str(error)}), 503

    @app.errorhandler(OSError)
    def unavailable(error: OSError) -> tuple[Response, int]:
        return jsonify({"error": str(error)}), 503

    @app.after_request
    def security_headers(response: Response) -> Response:
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
    def index() -> str:
        return render_template("index.html", dashboard_type="Python", app_version=__version__)

    @app.get("/console")
    def console_page() -> str:
        return render_template("console.html", dashboard_type="Python", app_version=__version__)

    @app.get("/favicon.svg")
    def favicon() -> Response:
        return send_from_directory(root, "favicon.svg", mimetype="image/svg+xml")

    @app.get("/css/<path:filename>")
    def css(filename: str) -> Response:
        return send_from_directory(root / "css", filename)

    @app.get("/js/<path:filename>")
    def javascript(filename: str) -> Response:
        return send_from_directory(root / "js", filename)


def _board(boards: DashboardBoards) -> DeviceMonitor:
    return boards.monitor(request.args.get("board"))


def _register_console_routes(app: Flask, boards: DashboardBoards, console: ConsoleBridge) -> None:
    @app.post("/api/console")
    def console_connect() -> Response | tuple[Response, int]:
        try:
            payload = request.get_json(silent=True)
            if not isinstance(payload, dict) or payload:
                return jsonify({
                    "error": "Expected an empty object; TCP authentication uses the connected board"
                }), 400
            board_id = request.args.get("board")
            monitor = boards.monitor(board_id)
            snapshot = monitor.snapshot()
            if (not snapshot["connected"] or snapshot.get("wireless_error")
                    or snapshot.get("wireless_cleanup_required")):
                raise BoardConflict("The board's wireless status is unavailable; refresh or clear the Wi-Fi test")
            wireless = snapshot.get("wireless")
            if wireless is None:
                raise BoardConflict("Waiting for the board's wireless status")
            return jsonify({
                "session": console.open(wireless, monitor.tcp_cli_auth_proof, board_id=board_id),
            })
        except OSError as error:
            app.logger.warning("TCP console connection failed: %s", error)
            return jsonify({"error": f"TCP console connection failed: {error}"}), 503

    @app.get("/api/console")
    def console_read() -> Response:
        data, closed = console.read(request.args.get("session"))
        return jsonify({"data": base64.b64encode(data).decode("ascii"), "closed": closed})

    @app.put("/api/console")
    def console_write() -> Response | tuple[Response, int]:
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
    def console_close() -> Response:
        console.close(request.args.get("session"))
        return jsonify({"ok": True})


def _register_api_routes(app: Flask, boards: DashboardBoards) -> None:
    @app.get("/api/status")
    def status() -> Response:
        board_id = request.args.get("board")
        if board_id is None:
            return jsonify(_empty_snapshot())
        snapshot = boards.monitor(board_id).status_snapshot()
        wireless = snapshot.get("wireless")
        if wireless is not None:
            return jsonify({**snapshot, "wireless": _public_wireless(wireless)})
        return jsonify(snapshot)

    @app.get("/api/boards")
    def boards_list() -> Response:
        return jsonify(boards.boards())

    @app.post("/api/led")
    def set_led() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("on"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'on' field"}), 400
        return jsonify(_board(boards).set_led(payload["on"]))

    @app.post("/api/settings")
    def set_settings() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected boot settings JSON"}), 400
        force = payload.pop("confirm_data_loss", False)
        if type(force) is not bool:
            return jsonify({"error": "confirm_data_loss must be boolean"}), 400
        _validate_settings(payload)
        return jsonify(_board(boards).set_settings(payload, force=force))

    @app.post("/api/msc")
    def set_msc() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or not isinstance(payload.get("enabled"), bool):
            return jsonify({"error": "Expected JSON with a boolean 'enabled' field"}), 400
        return jsonify(_board(boards).set_msc(payload["enabled"]))

    @app.post("/api/reboot")
    def reboot() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm") is not True:
            return jsonify({"error": "Explicit confirm: true is required"}), 400
        return jsonify(_board(boards).reboot())

    @app.post("/api/control/resync")
    def resync_control() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm") is not True:
            return jsonify({"error": "Explicit confirm: true is required"}), 400
        result = _board(boards).resync_command()
        wireless = result["wireless"]
        return jsonify({**result, "wireless": _public_wireless(wireless) if wireless is not None else None})

    @app.post("/api/self-test")
    def self_test() -> tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected a self-test JSON command"}), 400
        action = payload.get("action")
        if action not in ("start", "cancel"):
            return jsonify({"error": "Self-test action must be start or cancel"}), 400
        size_mib = None if action == "cancel" else payload.get("size_mib")
        build_self_test_command_report(action, size_mib)
        return jsonify(_board(boards).set_self_test(action, size_mib)), 202

    @app.post("/api/msc/force-return")
    def force_return_msc() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get("confirm_data_loss") is not True:
            return jsonify({"error": "Explicit confirm_data_loss: true is required"}), 400
        return jsonify(_board(boards).set_msc(False, force=True))


def _register_wireless_routes(app: Flask, boards: DashboardBoards) -> None:
    @app.post("/api/wireless/scan")
    def scan_wireless() -> Response | tuple[Response, int]:
        try:
            return jsonify(_board(boards).scan_wireless())
        except (OSError, ProtocolError):
            app.logger.exception("Wireless scan failed")
            return _unavailable("Wireless scan failed")

    @app.post("/api/wireless/test")
    def test_wireless() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or set(payload) - {"ssid", "ssid_hex", "password"}:
            return jsonify({"error": "Expected SSID and optional password"}), 400
        ssid = request_ssid(payload) if "ssid_hex" in payload else payload.get("ssid", "")
        build_wireless_command_report(1, ControlOpcode.STAGE_SSID, value=ssid)
        build_wireless_command_report(1, ControlOpcode.STAGE_PASSWORD, value=payload.get("password", ""))
        try:
            return jsonify(_board(boards).test_wireless(ssid, payload.get("password", "")))
        except TimeoutError:
            app.logger.exception("Wi-Fi test timed out")
            return _unavailable("Wi-Fi test timed out; confirm the result before retrying", uncertain=True)
        except (OSError, ProtocolError):
            app.logger.exception("Wi-Fi test is unavailable")
            return _unavailable("Wi-Fi test is unavailable")

    @app.post("/api/wireless/cancel")
    def cancel_wireless_test() -> Response | tuple[Response, int]:
        try:
            return jsonify(_board(boards).cancel_wireless_test())
        except (OSError, ProtocolError):
            app.logger.exception("Could not clear wireless test credentials")
            return _unavailable("Could not clear the wireless test credentials")

    @app.post("/api/wireless")
    def set_wireless() -> Response | tuple[Response, int]:
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"error": "Expected wireless configuration JSON"}), 400
        validate_wireless_configuration(payload)  # Bad input is 400 whatever the board state.
        try:
            result = _board(boards).set_wireless(payload)
            return jsonify({**result, "wireless": _public_wireless(result["wireless"])})
        except (OSError, ProtocolError):
            app.logger.exception("Wireless provisioning outcome is uncertain")
            return _unavailable("Wireless provisioning is unavailable")


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