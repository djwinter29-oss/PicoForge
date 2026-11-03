"""Compatibility imports for the historical monitor module."""

from .dashboard.monitor import DashboardBoards, DeviceMonitor, _board_status_from_runtime, _empty_snapshot

__all__ = ["DashboardBoards", "DeviceMonitor", "_board_status_from_runtime", "_empty_snapshot"]
