# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Typed data shapes shared by dashboard services and route registration."""

from __future__ import annotations

from typing import TypedDict

from ..reports.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    MemoryStatus,
    MscStatus,
    RuntimeStatus,
    SelfTestStatus,
    WirelessStatus,
)


class DashboardSnapshot(TypedDict):
    """Complete status published by `GET /api/status`; `_empty_snapshot()` fills every key."""

    connected: bool
    reconnecting: bool
    error: str | None
    hardware: HardwareIdentity | None
    board_configuration: BoardConfiguration | None
    runtime: RuntimeStatus | None
    memory: MemoryStatus | None
    self_test: SelfTestStatus | None
    settings_supported: bool
    settings_message: str | None
    control_result: ControlResult | None
    command_outcome_uncertain: bool
    reboot_pending: bool
    telemetry_stale: bool
    storage_stale: bool
    self_test_stale: bool
    updated_at: str | None
    wireless_phase: str | None
    wireless: WirelessStatus | None
    wireless_error: str | None
    wireless_cleanup_required: bool
    command_resync_supported: bool


class DashboardBoard(TypedDict):
    id: str
    product_name: str
    serial_number: str | None
    device_path: str


class _BoardWarnings(TypedDict, total=False):
    warnings: list[str]


class DashboardBoardsResponse(_BoardWarnings):
    boards: list[DashboardBoard]
    default_board_id: str | None


class WirelessSaveResponse(TypedDict):
    wireless: WirelessStatus
    reboot_pending: bool
    settings: BootSettings


class ControlResponse(TypedDict):
    control_result: ControlResult


class MscResponse(ControlResponse):
    msc: MscStatus


class SettingsResponse(TypedDict):
    settings: BootSettings
    control_result: ControlResult | None


class ResyncResponse(TypedDict):
    uncertain: bool
    control_result: ControlResult | None
    wireless: WirelessStatus | None
