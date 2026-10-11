#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
WEB_ROOT="$REPO_ROOT/host/webhid"
PYTHON="${PYTHON:-python3}"
BIND_ADDRESS="${BIND_ADDRESS:-127.0.0.1}"
PORT="${PORT:-8000}"

usage() {
    cat <<'EOF'
Usage: tools/host/run-webhid.sh [--bind ADDRESS] [--port PORT]

Serve host/webhid at the origin root for browser WebHID access.
Defaults: bind 127.0.0.1, port 8000. Set PYTHON to choose Python.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bind)
            if [ "$#" -lt 2 ]; then
                usage >&2
                exit 2
            fi
            BIND_ADDRESS="$2"
            shift 2
            ;;
        --port)
            if [ "$#" -lt 2 ]; then
                usage >&2
                exit 2
            fi
            PORT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Python interpreter not found: $PYTHON" >&2
    exit 1
fi
if [ ! -f "$WEB_ROOT/index.html" ]; then
    echo "WebHID index not found: $WEB_ROOT/index.html" >&2
    exit 1
fi

exec "$PYTHON" -m http.server "$PORT" --bind "$BIND_ADDRESS" --directory "$WEB_ROOT"