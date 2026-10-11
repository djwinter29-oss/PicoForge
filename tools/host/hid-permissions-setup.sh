#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
RULE_TARGET="/etc/udev/rules.d/99-picoforge-hid.rules"
USB_RULE='SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="cafe", ATTR{idProduct}=="4011", GROUP="plugdev", MODE="0660", TAG+="uaccess"'
HIDRAW_RULE='SUBSYSTEM=="hidraw", KERNEL=="hidraw*", ATTRS{idVendor}=="cafe", ATTRS{idProduct}=="4011", GROUP="plugdev", MODE="0660", TAG+="uaccess"'
RULE_CONTENT=$(printf '%s\n%s' "$USB_RULE" "$HIDRAW_RULE")
LEGACY_RULE='SUBSYSTEM=="hidraw", KERNEL=="hidraw*", ATTRS{idVendor}=="cafe", ATTRS{idProduct}=="4011", TAG+="uaccess"'
PREVIOUS_RULE="$HIDRAW_RULE"

case "${1:-}" in
    -h|--help)
        printf 'Usage: tools/host/hid-permissions-setup.sh\nInstall or update the PicoForge Linux HID udev rule. May prompt for sudo.\n'
        exit 0
        ;;
    "")
        ;;
    *)
        echo "Unknown argument: $1" >&2
        echo "Usage: tools/host/hid-permissions-setup.sh" >&2
        exit 2
        ;;
esac

if [ "$(uname -s)" != "Linux" ]; then
    echo "PicoForge HID udev permissions are only needed on Linux."
    exit 0
fi

if ! command -v udevadm >/dev/null 2>&1; then
    echo "udevadm is required to configure PicoForge HID permissions." >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "sudo is required to install the PicoForge HID udev rule." >&2
        exit 1
    fi
    exec sudo "$SCRIPT_DIR/hid-permissions-setup.sh" "$@"
fi

if [ -f "$RULE_TARGET" ]; then
    INSTALLED_RULE=$(cat "$RULE_TARGET")
    if [ "$INSTALLED_RULE" != "$RULE_CONTENT" ] &&
       [ "$INSTALLED_RULE" != "$LEGACY_RULE" ] &&
       [ "$INSTALLED_RULE" != "$PREVIOUS_RULE" ]; then
        echo "Existing udev rule differs from the known PicoForge rules; refusing to overwrite $RULE_TARGET." >&2
        echo "Review and merge the rule manually." >&2
        exit 1
    fi
fi

if [ ! -f "$RULE_TARGET" ] || [ "$INSTALLED_RULE" != "$RULE_CONTENT" ]; then
    TEMP_RULE=$(mktemp)
    trap 'rm -f "$TEMP_RULE"' 0 HUP INT TERM
    printf '%s\n' "$RULE_CONTENT" > "$TEMP_RULE"
    install -m 0644 "$TEMP_RULE" "$RULE_TARGET"
fi

udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=cafe --attr-match=idProduct=4011
udevadm trigger --subsystem-match=hidraw
udevadm settle
echo "PicoForge HID permissions applied. Reconnect the board if access is still denied."