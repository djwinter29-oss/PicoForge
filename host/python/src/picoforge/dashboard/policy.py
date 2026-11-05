"""Dashboard operation gates derived from the latest published snapshot."""

from __future__ import annotations

from .models import DashboardSnapshot


def require_wireless_idle(
    snapshot: DashboardSnapshot,
    *,
    client_connected: bool,
    command_outcome_uncertain: bool,
) -> None:
    hardware = snapshot.get("hardware")
    if (not client_connected or not snapshot.get("connected")
            or not isinstance(hardware, dict) or not hardware.get("wireless_supported")):
        raise RuntimeError("Wireless provisioning is unavailable on this firmware")
    if command_outcome_uncertain or snapshot.get("command_outcome_uncertain"):
        raise RuntimeError("A prior command outcome is uncertain; no further writes are allowed")

    msc = snapshot.get("msc")
    freshness = snapshot.get("report_freshness")
    storage_report = freshness.get("10") if isinstance(freshness, dict) else None
    self_test_report = freshness.get("11") if isinstance(freshness, dict) else None
    self_test = snapshot.get("self_test")
    control_result = snapshot.get("control_result")
    if (not isinstance(storage_report, dict) or storage_report.get("available") is not True
            or storage_report.get("stale") is not False
            or snapshot.get("storage_stale") is not False
            or not isinstance(msc, dict) or msc.get("enabled") or msc.get("transition_pending")
            or snapshot.get("reboot_pending")
            or isinstance(control_result, dict) and control_result.get("status") in ("pending", "busy")
            or hardware.get("self_test_supported") and (
                not isinstance(self_test, dict) or not isinstance(self_test_report, dict)
                or self_test_report.get("available") is not True or self_test_report.get("stale") is not False
                or snapshot.get("self_test_stale") is not False)
            or isinstance(self_test, dict) and self_test.get("active")):
        raise RuntimeError("Wireless scan and connection test require idle firmware-owned storage")
