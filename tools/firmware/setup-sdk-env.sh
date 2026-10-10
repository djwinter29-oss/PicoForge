#!/usr/bin/env sh
set -eu

SCRIPT_PATH="${BASH_SOURCE:-$0}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$SCRIPT_PATH")" && pwd)
REPO_ROOT="${PICO_FORGE_REPO_ROOT:-$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)}"

if [ ! -d "$REPO_ROOT/firmware" ]; then
    echo "Repository root not found from $SCRIPT_DIR." >&2
    return 1 2>/dev/null || exit 1
fi

PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-$REPO_ROOT/firmware/third_party/pico-sdk}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --sdk-path)
            PICO_SDK_PATH_VALUE="$2"
            shift 2
            ;;
        --help)
            cat <<'EOF'
Usage: . tools/firmware/setup-sdk-env.sh [options]

Options:
    --sdk-path PATH       Use an existing SDK checkout instead of the submodule

Environment override: PICO_SDK_PATH
Source this script to retain PICO_SDK_PATH in the current shell.
EOF
            return 0 2>/dev/null || exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            return 1 2>/dev/null || exit 1
            ;;
    esac
done

if ! command -v git >/dev/null 2>&1; then
    echo "git is required to verify the Pico SDK checkout." >&2
    return 1 2>/dev/null || exit 1
fi

if [ ! -d "$PICO_SDK_PATH_VALUE" ]; then
    echo "Pico SDK is not available at $PICO_SDK_PATH_VALUE." >&2
    echo "Initialize submodules with 'git submodule update --init --recursive'." >&2
    return 1 2>/dev/null || exit 1
fi

if ! git -C "$PICO_SDK_PATH_VALUE" rev-parse --git-dir >/dev/null 2>&1; then
    echo "Pico SDK path is not a Git checkout: $PICO_SDK_PATH_VALUE" >&2
    return 1 2>/dev/null || exit 1
fi

CURRENT_SDK_REVISION=$(git -C "$PICO_SDK_PATH_VALUE" rev-parse HEAD 2>/dev/null)
EXPECTED_SDK_REVISION=$(git -C "$REPO_ROOT" ls-files --stage -- firmware/third_party/pico-sdk | awk '$1 == "160000" { print $2 }')
if [ "$PICO_SDK_PATH_VALUE" = "$REPO_ROOT/firmware/third_party/pico-sdk" ] &&
    [ -n "$EXPECTED_SDK_REVISION" ] && [ "$CURRENT_SDK_REVISION" != "$EXPECTED_SDK_REVISION" ]; then
    echo "Pico SDK submodule revision mismatch: expected $EXPECTED_SDK_REVISION, got $CURRENT_SDK_REVISION." >&2
    return 1 2>/dev/null || exit 1
fi

if ! git -C "$PICO_SDK_PATH_VALUE" submodule update --init --recursive; then
    echo "Pico SDK submodule initialization failed." >&2
    return 1 2>/dev/null || exit 1
fi
if [ -n "$(git -C "$PICO_SDK_PATH_VALUE" status --porcelain --untracked-files=no)" ]; then
    echo "Pico SDK checkout or submodules contain tracked modifications." >&2
    return 1 2>/dev/null || exit 1
fi
if [ ! -f "$PICO_SDK_PATH_VALUE/external/pico_sdk_import.cmake" ]; then
    echo "Pico SDK checkout is incomplete: $PICO_SDK_PATH_VALUE" >&2
    return 1 2>/dev/null || exit 1
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "arm-none-eabi-gcc is not available on PATH." >&2
    return 1 2>/dev/null || exit 1
fi

export PICO_SDK_PATH="$PICO_SDK_PATH_VALUE"
echo "PICO_SDK_PATH=$PICO_SDK_PATH"
echo "Pico SDK revision: $CURRENT_SDK_REVISION"