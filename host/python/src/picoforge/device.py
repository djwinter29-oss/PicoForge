"""Compatibility imports for the historical device module."""

from .hid.device import PicoForgeHid, enumerate_devices
from .hid.device import _matches_vendor_collection

__all__ = ["PicoForgeHid", "enumerate_devices", "_matches_vendor_collection"]
