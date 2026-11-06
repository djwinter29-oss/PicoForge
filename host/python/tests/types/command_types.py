# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Compile-only contracts: suppressions must correspond to real argument errors."""

# pyright: reportPrivateUsage=false

from picoforge.dashboard.console import ConsoleBridge
from picoforge.dashboard.boards import DashboardBoards
from picoforge.dashboard.models import ControlResponse, DashboardSnapshot, MscResponse, ResyncResponse, SettingsResponse
from picoforge.dashboard.monitor import DeviceMonitor
from picoforge.dashboard.server import _board, create_app
from flask import Flask
from picoforge.hid.device import PicoForgeHid
from picoforge.reports.models import ControlResult, HardwareIdentity, InputReport, WirelessStatus, WirelessTestResult


def check_command_types(client: PicoForgeHid, monitor: DeviceMonitor, console: ConsoleBridge) -> None:
    _led: ControlResult = monitor._send_control(client.set_led, True)
    _wireless: WirelessTestResult = monitor._send_control(client.test_wireless, b"raw SSID", "password")
    _read: tuple[bytes, bool] = console.read(None)
    _control: ControlResponse = monitor.set_led(True)
    _msc: MscResponse = monitor.set_msc(True)
    _settings: SettingsResponse = monitor.set_settings({})
    _resync: ResyncResponse = monitor.resync_command()
    _invalid: int = monitor.set_led(True)["control_result"]["status"]  # pyright: ignore[reportAssignmentType]
    console.write(None, b"help\r")
    monitor._send_control(client.set_led, "on")  # pyright: ignore[reportArgumentType]
    monitor._send_control(client.test_wireless, "SSID", cancel=True)  # pyright: ignore[reportArgumentType]
    client.set_msc("on")  # pyright: ignore[reportArgumentType]
    console.write(None, "help")  # pyright: ignore[reportArgumentType]


def check_route_types(boards: DashboardBoards) -> None:
    _app: Flask = create_app(boards)
    _monitor: DeviceMonitor = _board(boards)
    _default: str | None = boards.boards()["default_board_id"]
    create_app("boards")  # pyright: ignore[reportArgumentType]
    _board(None)  # pyright: ignore[reportArgumentType]
    _invalid: str = _board(boards).set_msc(True)["msc"]["enabled"]  # pyright: ignore[reportAssignmentType]


def check_lifecycle_types(client: PicoForgeHid, monitor: DeviceMonitor, result: ControlResult) -> None:
    _snapshot: DashboardSnapshot = monitor.snapshot()
    _cleanup: bool = _snapshot["wireless_cleanup_required"]
    _identity: HardwareIdentity = client.read_info()
    _input: InputReport | None = client.read_input_report(timeout_ms=0)
    _status: WirelessStatus = client.read_wireless()
    _command: tuple[int, int] | None = client.uncertain_command
    _wireless: tuple[str, int] | None = client.uncertain_wireless_operation
    _close_error: Exception | None = client._close_error
    _closed: bool = client.closed
    monitor._record_uncertain_command(client)
    monitor._publish_control_result(result)
    client.require_completed(result)
    client.close()
    client.read_input_report(timeout_ms="0")  # pyright: ignore[reportArgumentType]
    client.close(True)  # pyright: ignore[reportCallIssue]
    monitor.set_identity_ambiguous("yes")  # pyright: ignore[reportArgumentType]
    _snapshot["wireless_cleanup_required"] = "yes"  # pyright: ignore[reportGeneralTypeIssues]
    _invalid: tuple[str, int] | None = client.uncertain_command  # pyright: ignore[reportAssignmentType]
    monitor._publish_control_result({"status": "completed"})  # pyright: ignore[reportArgumentType]
    client._close_error = "failed"  # pyright: ignore[reportAttributeAccessIssue]
