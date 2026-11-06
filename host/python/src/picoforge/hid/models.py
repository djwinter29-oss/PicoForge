"""Typed HID input report envelopes."""

from __future__ import annotations

from typing import Literal, TypedDict


class SupportedHardwareIdentity(TypedDict):
    version: int
    product_id: int
    product_name: str
    supported: Literal[True]
    device_uid: str
    serial_number: str
    firmware_major: int
    firmware_minor: int
    firmware_patch: int
    firmware_version: str
    mcu_id: int
    mcu: str
    board_model_id: int
    board_model: str
    capabilities: int
    led_available: bool
    reboot_available: bool
    settings_supported: bool
    storage_transport: str
    self_test_supported: bool
    wireless_supported: bool
    ram_total_bytes: int
    ram_reserved_bytes: int


class UnsupportedHardwareIdentity(TypedDict):
    version: int
    product_id: int
    product_name: str
    supported: Literal[False]
    capabilities: int
    led_available: bool
    reboot_available: bool
    settings_supported: bool
    self_test_supported: bool
    wireless_supported: bool


HardwareIdentity = SupportedHardwareIdentity | UnsupportedHardwareIdentity


class BoardConfiguration(TypedDict):
    version: int
    stored_sysclk_profile: int
    system_clock_mhz: int
    stored_sdcard_profile: int
    sdcard_profile: int
    radios: int
    tcp_cli_enabled: bool
    monitor_uart_index: int
    monitor_uart_tx_gpio: int
    monitor_uart_rx_gpio: int
    storage_gpios: tuple[int, ...]
    board_flags: int
    reset_cause: int
    led_gpio: int
    card_detect_gpio: int
    storage_transport: str


class BootSettings(TypedDict):
    system_clock_mhz: int
    sdcard_profile: int
    radios: int
    tcp_cli_enabled: bool


class RuntimeStatus(TypedDict):
    version: int
    sequence: int
    uptime_ms: int
    system_clock_hz: int
    usb_mounted: bool
    cdc_open: bool
    temperature_valid: bool
    temperature_centidegrees: int | None
    led_available: bool
    led_on: bool
    led_flags: int
    picoforge_flags: int
    cpu_usage_core0_basis_points: int | None
    cpu_usage_core1_basis_points: int | None
    ram_used_bytes: int | None
    ram_total_bytes: int | None


class MscStatus(TypedDict):
    enabled: bool
    transition_pending: bool
    request_rejected: bool


class SdCardIdentity(TypedDict):
    id: int
    oem_id: str
    product_name: str
    product_revision: str
    serial_number: int
    manufacture_year: int
    manufacture_month: int
    raw: str


class MemoryStatus(TypedDict):
    version: int
    sequence: int
    configured: bool
    sample_valid: bool
    presence_known: bool
    present: bool | None
    initialized: bool
    component_type: str | None
    mode: str | None
    bus_clock_hz: int | None
    speed_mode: str | None
    block_count: int | None
    capacity_bytes: int | None
    cid: str | None
    manufacturer: SdCardIdentity | None
    filesystem_format: str
    msc_enabled: bool
    msc_transition_pending: bool
    msc_request_rejected: bool
    msc: MscStatus


class SelfTestStatus(TypedDict):
    version: int
    sequence: int
    state_id: int
    state: str
    active: bool
    target_bytes: int
    written_bytes: int
    read_bytes: int
    write_elapsed_ms: int
    read_elapsed_ms: int
    write_bytes_per_second: float | None
    read_bytes_per_second: float | None
    progress_percent: float
    mismatch_count: int
    error: int
    error_name: str
    filesystem_result: int
    file_remains: bool
    volume_mounted: bool


class WirelessReportStatus(TypedDict):
    wifi_state: str
    wifi_state_id: int
    ipv4: str
    ssid: str
    ble_key_configured: bool
    ble_state: str
    tcp_cli_started: bool
    tcp_cli_enabled: bool
    ssid_hex: str
    detail: int
    phase: str
    network_configured: bool
    ssid_staged: bool
    password_staged: bool
    key_staged: bool
    pair_tested: bool
    scan_state: str
    scan_generation: int
    scan_count: int
    test_generation: int
    staged_state: int


class WirelessStatus(WirelessReportStatus):
    wifi_enabled: bool
    ble_enabled: bool
    tcp_cli_enabled: bool
    stale: bool
    remaining_seconds: int | None


class WirelessSaveResult(WirelessStatus):
    settings: BootSettings


class WirelessNetwork(TypedDict):
    ssid: str
    rssi: int
    secured: bool
    security: str
    ssid_hex: str
    generation: int
    index: int


class WirelessTestResult(TypedDict):
    ipv4: str
    detail: int
    phase: str
    test_generation: int
    remaining_seconds: int


class ControlResult(TypedDict):
    version: int
    transaction_id: int
    opcode: int
    status_id: int
    status: str
    error_reason: int
    error: str
    saved_settings: BootSettings | None
    detail: int
    commit_mask: int
    scan_entry: WirelessNetwork | None
    proof: bytes | None


class RuntimeInputReport(TypedDict):
    kind: Literal["runtime"]
    data: RuntimeStatus


class MemoryInputReport(TypedDict):
    kind: Literal["memory"]
    data: MemoryStatus


class SelfTestInputReport(TypedDict):
    kind: Literal["self_test"]
    data: SelfTestStatus


InputReport = RuntimeInputReport | MemoryInputReport | SelfTestInputReport
