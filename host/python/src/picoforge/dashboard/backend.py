"""Dashboard service contract consumed by HTTP routes."""

from __future__ import annotations

from typing import Protocol

from .models import (
    BoardStatus,
    BootSettings,
    ControlResult,
    DashboardBoardsResponse,
    DashboardSnapshot,
    MscStatus,
    WirelessCancelResponse,
    WirelessDashboardResponse,
    WirelessSaveResponse,
    WirelessScanResponse,
    WirelessTestResponse,
)


class DashboardBackend(Protocol):
    def boards(self) -> DashboardBoardsResponse: ...

    def snapshot(self, board_id: str | None = None) -> DashboardSnapshot: ...

    def set_led(self, board_id: str | None, on: bool) -> BoardStatus: ...

    def read_wireless(self, board_id: str | None) -> WirelessDashboardResponse: ...

    def scan_wireless(self, board_id: str | None) -> WirelessScanResponse: ...

    def test_wireless(
        self, board_id: str | None, ssid: str, password: str = ""
    ) -> WirelessTestResponse: ...

    def cancel_wireless_test(self, board_id: str | None) -> WirelessCancelResponse: ...

    def set_wireless(self, board_id: str | None, settings: dict[str, object]) -> WirelessSaveResponse: ...

    def set_settings(
        self, board_id: str | None, settings: dict[str, object], *, force: bool = False
    ) -> BootSettings: ...

    def set_msc(
        self, board_id: str | None, enabled: bool, *, force: bool = False
    ) -> MscStatus: ...

    def reboot(self, board_id: str | None) -> ControlResult: ...

    def set_self_test(self, board_id: str | None, action: str, size_mib: int | None) -> None: ...
