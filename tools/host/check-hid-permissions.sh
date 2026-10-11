#!/usr/bin/env sh
set -eu

if [ "$(uname -s)" != "Linux" ]; then
    exit 0
fi

if ! command -v udevadm >/dev/null 2>&1; then
    exit 0
fi

for device_node in /dev/hidraw* /dev/bus/usb/*/*; do
    [ -e "$device_node" ] || continue
    properties=$(udevadm info --query=property --name="$device_node" 2>/dev/null || true)
    printf '%s\n' "$properties" | grep -Fqx 'ID_VENDOR_ID=cafe' || continue
    printf '%s\n' "$properties" | grep -Fqx 'ID_MODEL_ID=4011' || continue

    if [ ! -r "$device_node" ] || [ ! -w "$device_node" ]; then
        echo "No read/write permission for PicoForge device node: $device_node" >&2
        echo "Run tools/host/setup-hid-permissions.sh, then reconnect the Pico." >&2
        exit 1
    fi
done