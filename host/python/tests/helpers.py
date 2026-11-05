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
    monitor = DeviceMonitor(client_factory=lambda: client, autostart=False)
    monitor._active_client = client
    if snapshot is not None:
        monitor._snapshot = {**_empty_snapshot(), **snapshot}
    if fresh:
        monitor._input_received_at = dict.fromkeys((3, 10, 11), math.inf)
    return monitor
