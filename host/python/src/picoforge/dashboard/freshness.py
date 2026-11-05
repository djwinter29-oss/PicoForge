"""Report freshness calculation for dashboard snapshots."""

from __future__ import annotations

from collections.abc import Mapping

from .models import ReportFreshness

_REPORT_INTERVALS = {3: 0.750, 10: 1.500, 11: 1.500}


def calculate_report_freshness(
    received_at: Mapping[int, float],
    now: float,
    connected: bool,
    hardware_supported: bool | None,
    self_test_supported: bool | None,
) -> dict[str, ReportFreshness]:
    """Return report availability and age without mutating monitor state."""
    freshness: dict[str, ReportFreshness] = {}
    for report_id, interval in _REPORT_INTERVALS.items():
        received = received_at.get(report_id)
        supported = self_test_supported if report_id == 11 else (
            hardware_supported if connected else None
        )
        age_ms = max(0, int((now - received) * 1000)) if received is not None else None
        freshness[str(report_id)] = {
            "supported": supported,
            "available": received is not None,
            "stale": received is not None and now - received >= interval,
            "age_ms": age_ms,
        }
    return freshness
