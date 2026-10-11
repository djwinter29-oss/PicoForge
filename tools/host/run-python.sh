#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PYTHON_EXE="${PYTHON:-}"

if [ -z "$PYTHON_EXE" ]; then
    if [ -x "$REPO_ROOT/build/picoforge/bin/python" ]; then
        PYTHON_EXE="$REPO_ROOT/build/picoforge/bin/python"
    else
        PYTHON_EXE="python3"
    fi
fi

if ! command -v "$PYTHON_EXE" >/dev/null 2>&1; then
    echo "Python interpreter not found: $PYTHON_EXE" >&2
    echo "Install host/python dependencies or set PYTHON to the interpreter path." >&2
    exit 1
fi

if ! "$PYTHON_EXE" -c 'import flask' >/dev/null 2>&1; then
    echo "The selected Python environment is missing Flask or the PicoForge host dependencies." >&2
    echo "Set them up with: uv venv build/picoforge && uv pip install --python build/picoforge/bin/python -e host/python" >&2
    echo "Then run this script again (or set PYTHON to that environment's interpreter)." >&2
    exit 1
fi

APP_COMMAND="${1:-web}"
if [ "$#" -gt 0 ]; then
    shift
fi

if [ "$(uname -s)" = "Linux" ]; then
    rule_source="$SCRIPT_DIR/99-picoforge-hid.rules"
    rule_target="/etc/udev/rules.d/99-picoforge-hid.rules"
    if [ ! -f "$rule_source" ]; then
        echo "PicoForge udev rule not found: $rule_source" >&2
        exit 1
    fi

    rule_line=$(cat "$rule_source")
    if [ -f "$rule_target" ]; then
        if ! grep -Fqx "$rule_line" "$rule_target"; then
            echo "Existing udev rule differs from $rule_source; refusing to overwrite $rule_target." >&2
            echo "Review and merge the PicoForge rule manually." >&2
            exit 1
        fi
    else
        if ! command -v udevadm >/dev/null 2>&1; then
            echo "udevadm is required to install HID permissions on Linux." >&2
            exit 1
        fi
        if [ "$(id -u)" -eq 0 ]; then
            install -m 0644 "$rule_source" "$rule_target"
            udevadm control --reload-rules
            udevadm trigger --subsystem-match=hidraw
            udevadm settle
        else
            if ! command -v sudo >/dev/null 2>&1; then
                echo "sudo is required to install the PicoForge HID udev rule." >&2
                exit 1
            fi
            sudo install -m 0644 "$rule_source" "$rule_target"
            sudo udevadm control --reload-rules
            sudo udevadm trigger --subsystem-match=hidraw
            sudo udevadm settle
        fi
    fi
fi

PYTHONPATH="$REPO_ROOT/host/python/src${PYTHONPATH:+:$PYTHONPATH}" \
    exec "$PYTHON_EXE" -m picoforge "$APP_COMMAND" "$@"