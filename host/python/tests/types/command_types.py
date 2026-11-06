"""Compile-only contracts: suppressions must correspond to real argument errors."""

# pyright: reportPrivateUsage=false

from picoforge.dashboard.console import ConsoleBridge
from picoforge.dashboard.models import ControlResponse, MscResponse, ResyncResponse, SettingsResponse
from picoforge.dashboard.monitor import DeviceMonitor
from picoforge.hid.device import PicoForgeHid
from picoforge.reports.models import ControlResult, WirelessTestResult


def check_command_types(client: PicoForgeHid, monitor: DeviceMonitor, console: ConsoleBridge) -> None:
    _led: ControlResult = monitor._send_control(client.set_led, True)
    _wireless: WirelessTestResult = monitor._send_control(client.test_wireless, b"raw SSID", "password")
    _read: tuple[bytes, bool] = console.read(None)
    _control: ControlResponse = monitor.set_led(True)
    _msc: MscResponse = monitor.set_msc(True)
    _settings: SettingsResponse = monitor.set_settings({})
    _resync: ResyncResponse = monitor.resync_command()
    _invalid: int = monitor.set_led(True)["control_result"]["status"]  # pyright: ignore[reportAssignmentType]
    console.write(None, b"help\r")
    monitor._send_control(client.set_led, "on")  # pyright: ignore[reportArgumentType]
    monitor._send_control(client.test_wireless, "SSID", cancel=True)  # pyright: ignore[reportArgumentType]
    client.set_msc("on")  # pyright: ignore[reportArgumentType]
    console.write(None, "help")  # pyright: ignore[reportArgumentType]
