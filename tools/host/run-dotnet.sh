#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
DOTNET_EXE="${DOTNET:-dotnet}"
PROJECT="$REPO_ROOT/host/dotnet/PicoForge/PicoForge.csproj"

usage() {
    cat <<'EOF'
Usage: tools/host/run-dotnet.sh [application arguments...]

Run the PicoForge .NET host dashboard at http://127.0.0.1:8766/.
Set DOTNET to select a .NET 10 SDK executable.
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

if ! command -v "$DOTNET_EXE" >/dev/null 2>&1; then
    echo ".NET SDK executable not found: $DOTNET_EXE" >&2
    echo "Install .NET 10 or set DOTNET to its executable path." >&2
    exit 1
fi

if [ ! -f "$PROJECT" ]; then
    echo "PicoForge .NET project not found: $PROJECT" >&2
    exit 1
fi

exec "$DOTNET_EXE" run \
    --project "$PROJECT" \
    --no-launch-profile \
    -- "$@"