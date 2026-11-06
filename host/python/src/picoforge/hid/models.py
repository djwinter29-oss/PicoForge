# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Compatibility exports for report shapes now owned by picoforge.reports.models."""

from ..reports.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    HardwareIdentity,
    InputReport,
    MemoryInputReport,
    MemoryStatus,
    MscStatus,
    RuntimeInputReport,
    RuntimeStatus,
    SdCardIdentity,
    SelfTestInputReport,
    SelfTestStatus,
    SupportedHardwareIdentity,
    UnsupportedHardwareIdentity,
    WirelessNetwork,
    WirelessReportStatus,
    WirelessSaveResult,
    WirelessStatus,
    WirelessTestResult,
)

__all__ = [
    "BoardConfiguration", "BootSettings", "ControlResult", "HardwareIdentity", "InputReport",
    "MemoryInputReport", "MemoryStatus", "MscStatus", "RuntimeInputReport", "RuntimeStatus",
    "SdCardIdentity", "SelfTestInputReport", "SelfTestStatus", "SupportedHardwareIdentity",
    "UnsupportedHardwareIdentity", "WirelessNetwork", "WirelessReportStatus", "WirelessSaveResult",
    "WirelessStatus", "WirelessTestResult",
]
