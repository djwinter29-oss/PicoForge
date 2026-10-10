#!/usr/bin/env sh
set -eu

BOARD="${PICO_BOARD:-pico}"
BUILD_DIR=""
GENERATOR="${CMAKE_GENERATOR:-}"
PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --board)
            BOARD="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --generator)
            GENERATOR="$2"
            shift 2
            ;;
        --pico-sdk-path)
            PICO_SDK_PATH_VALUE="$2"
            shift 2
            ;;
        --help)
            cat <<'EOF'
Usage: tools/firmware/build.sh [options]

Options:
  --board NAME       Pico SDK board (default: pico)
  --build-dir PATH   Build directory (default: build/firmware-NAME)
  --generator NAME  CMake generator (default: Ninja when available)
  --pico-sdk-path PATH
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_DIR="$REPO_ROOT/firmware"

if [ ! -f "$SOURCE_DIR/CMakeLists.txt" ]; then
    echo "Firmware CMake project not found: $SOURCE_DIR/CMakeLists.txt" >&2
    echo "Add the firmware build configuration before running this command." >&2
    exit 1
fi

if [ -z "$PICO_SDK_PATH_VALUE" ]; then
    PICO_SDK_PATH_VALUE="$REPO_ROOT/.pico-sdk"
fi
if [ ! -f "$PICO_SDK_PATH_VALUE/external/pico_sdk_import.cmake" ]; then
    echo "Pico SDK is not available at $PICO_SDK_PATH_VALUE." >&2
    echo "Run '. tools/firmware/setup-sdk-env.sh' first, or pass --pico-sdk-path." >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required to build firmware." >&2
    exit 1
fi

if [ -z "$GENERATOR" ]; then
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR="Ninja"
    else
        GENERATOR="Unix Makefiles"
    fi
fi
if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/build/firmware-$BOARD"
elif [ "${BUILD_DIR#/}" = "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DPICO_SDK_PATH="$PICO_SDK_PATH_VALUE" \
    -DPICO_BOARD="$BOARD"
cmake --build "$BUILD_DIR" --parallel