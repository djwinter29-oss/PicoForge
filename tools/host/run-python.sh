#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
VENV_DIR="${VENV_DIR:-$REPO_ROOT/build/picoforge}"
BASE_PYTHON="${PYTHON:-python3}"

if ! command -v "$BASE_PYTHON" >/dev/null 2>&1; then
    echo "Python interpreter not found: $BASE_PYTHON" >&2
    exit 1
fi

if ! "$BASE_PYTHON" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))'; then
    echo "PicoForge host tools require Python 3.10 or newer ($BASE_PYTHON)." >&2
    exit 1
fi

if [ ! -x "$VENV_DIR/bin/python" ]; then
    if command -v uv >/dev/null 2>&1; then
        uv venv --python "$BASE_PYTHON" "$VENV_DIR"
    else
        "$BASE_PYTHON" -m venv "$VENV_DIR"
    fi
fi

PYTHON_EXE="$VENV_DIR/bin/python"
if ! "$PYTHON_EXE" -c 'import flask, hid' >/dev/null 2>&1; then
    if command -v uv >/dev/null 2>&1; then
        uv pip install --python "$PYTHON_EXE" --editable "$REPO_ROOT/host/python"
    else
        "$PYTHON_EXE" -m pip install --disable-pip-version-check --no-input --editable "$REPO_ROOT/host/python"
    fi
fi

APP_COMMAND="${1:-web}"
if [ "$#" -gt 0 ]; then
    shift
fi

"$SCRIPT_DIR/hid-permissions-check.sh"

PYTHONPATH="$REPO_ROOT/host/python/src${PYTHONPATH:+:$PYTHONPATH}" \
    exec "$PYTHON_EXE" -m picoforge "$APP_COMMAND" "$@"