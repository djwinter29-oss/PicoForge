# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Shared dashboard test fixtures."""

import math
from unittest.mock import Mock

from picoforge.dashboard.monitor import DeviceMonitor, _empty_snapshot


def make_monitor(client=None, snapshot=None, *, fresh=True):
    """Build an unstarted monitor; fresh=True marks every input report as just received."""
    if isinstance(client, Mock):
        client.command_uncertain = False
        client.last_command_result = None
        client.wireless_active = False
        client.uncertain_command = None
        client.uncertain_wireless_operation = None
    monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)
    monitor._active_client = client
    if snapshot is not None:
        monitor._snapshot = {**_empty_snapshot(), **snapshot}
    if fresh:
        monitor._input_received_at = dict.fromkeys(("runtime", "memory", "self_test"), math.inf)
        monitor._wireless_received_at = math.inf
    return monitor
