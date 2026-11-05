"""Typed data shapes shared by dashboard services and route registration."""

from __future__ import annotations

from typing import TypedDict

from ..hid.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    MemoryStatus,
    MscStatus,
    RuntimeStatus,
    SelfTestStatus,
    WirelessNetwork,
    WirelessOperation,
    WirelessStatus,
)


class ReportFreshness(TypedDict):
    supported: bool | None
    available: bool
    stale: bool
    age_ms: int | None


class BoardStatus(TypedDict, total=False):
    temperature_valid: bool | None
    temperature_centidegrees: int | None
    led_available: bool
    led_on: bool


class DashboardSnapshot(TypedDict, total=False):
    connected: bool
    reconnecting: bool
    error: str | None
    hardware: HardwareIdentity | None
    board_configuration: BoardConfiguration | None
    runtime: RuntimeStatus | None
    board_status: BoardStatus | None
    memory: MemoryStatus | None
    filesystem_format: str | None
    msc: MscStatus | None
    self_test: SelfTestStatus | None
    settings: BootSettings | None
    settings_supported: bool
    settings_message: str | None
    control_result: ControlResult | None
    command_outcome_uncertain: bool
    reboot_pending: bool
    report_freshness: dict[str, ReportFreshness]
    telemetry_stale: bool
    storage_stale: bool
    self_test_stale: bool | None
    updated_at: str | None
    wireless_phase: str | None
    wireless: WirelessStatus | None
    wireless_error: str | None
    wireless_freshness: ReportFreshness


class DashboardBoard(TypedDict):
    id: str
    product_name: str
    serial_number: str | None
    device_path: str


class DashboardBoardsResponse(TypedDict):
    boards: list[DashboardBoard]
    default_board_id: str | None


class WirelessDashboardResponse(TypedDict):
    wireless: WirelessStatus
    reboot_pending: bool


class WirelessScanResponse(TypedDict):
    networks: list[WirelessNetwork]


class WirelessTestResponse(TypedDict):
    test: WirelessOperation


class WirelessSaveResponse(TypedDict):
    wireless: WirelessStatus
    reboot_pending: bool
    settings: BootSettings


class WirelessCancelResponse(TypedDict):
    cancelled: bool
