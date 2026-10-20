#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PYTHON=${PYTHON:-python3}
NODE=${NODE:-node}
DOTNET=${DOTNET:-dotnet}

for tool in "$PYTHON" "$NODE" "$DOTNET"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Required host test tool not found: %s\n' "$tool" >&2
        exit 1
    fi
done

printf '%s\n' '=== Python host tests ==='
"$PYTHON" -m unittest discover -s "$REPO_ROOT/host/python/tests" -v

printf '%s\n' '=== WebHID tests ==='
"$NODE" --test "$REPO_ROOT"/host/webhid/tests/*.test.mjs

printf '%s\n' '=== .NET host tests ==='
"$DOTNET" test "$REPO_ROOT/host/dotnet/PicoForge.sln" \
    --configuration Release --nologo