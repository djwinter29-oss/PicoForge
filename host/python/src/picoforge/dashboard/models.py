"""Typed data shapes shared by dashboard services and route registration."""

from __future__ import annotations

from typing import TypedDict

from ..hid.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    MemoryStatus,
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
    command_resync_supported: bool


class DashboardBoard(TypedDict):
    id: str
    product_name: str
    serial_number: str | None
    device_path: str


class DashboardBoardsResponse(TypedDict):
    boards: list[DashboardBoard]
    default_board_id: str | None


class WirelessSaveResponse(TypedDict):
    wireless: WirelessStatus
    reboot_pending: bool
    settings: BootSettings
