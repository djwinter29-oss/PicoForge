#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
FIRMWARE_BUILD_SCRIPT="$REPO_ROOT/tools/firmware/build.sh"
CEEDLING_SCRIPT="$REPO_ROOT/tools/validation/setup-ceedling-testing.sh"
PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-$REPO_ROOT/firmware/third_party/pico-sdk}"
GENERATOR="${CMAKE_GENERATOR:-}"
BOARD=""
SKIP_FIRMWARE=0
SKIP_TESTS=0
SKIP_SHELL=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --board)
            if [ "$#" -lt 2 ]; then
                echo "--board requires a board name." >&2
                exit 1
            fi
            BOARD="$2"
            shift 2
            ;;
        --pico-sdk-path)
            if [ "$#" -lt 2 ]; then
                echo "--pico-sdk-path requires a path." >&2
                exit 1
            fi
            PICO_SDK_PATH_VALUE="$2"
            shift 2
            ;;
        --generator)
            if [ "$#" -lt 2 ]; then
                echo "--generator requires a CMake generator." >&2
                exit 1
            fi
            GENERATOR="$2"
            shift 2
            ;;
        --skip-firmware)
            SKIP_FIRMWARE=1
            shift
            ;;
        --skip-tests)
            SKIP_TESTS=1
            shift
            ;;
        --skip-shell)
            SKIP_SHELL=1
            shift
            ;;
        --help|-h)
            cat <<'EOF'
Usage: tools/validation/validate.sh [options]

By default, syntax-check shell scripts, validate/run Ceedling tests, and build
firmware for pico and pico2.

Options:
  --board NAME          Build only the selected Pico SDK board
  --pico-sdk-path PATH  Use an explicit Pico SDK checkout
  --generator NAME      Select a CMake generator
  --skip-firmware       Skip Pico SDK firmware builds
  --skip-tests          Skip Ceedling configuration and unit tests
  --skip-shell          Skip shell-script syntax checks
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

cd "$REPO_ROOT"

if [ "$SKIP_SHELL" -eq 0 ]; then
    echo "=== Shell syntax ==="
    for script_dir in tools/firmware tools/host tools/validation; do
        for script in "$script_dir"/*.sh; do
            [ -f "$script" ] || continue
            first_line=$(sed -n '1p' "$script")
            case "$first_line" in
                '#!/usr/bin/env bash'|'#!/bin/bash')
                    bash -n "$script"
                    ;;
                *)
                    sh -n "$script"
                    ;;
            esac
        done
    done
fi

if [ "$SKIP_TESTS" -eq 0 ]; then
    echo "=== Ceedling project and unit tests ==="
    python3 tools/validation/check-storage-automount.py
    python3 tools/validation/check-sdio-init.py
    python3 tools/validation/check-sdio-read-batch.py
    python3 tools/validation/check-sd-spi-transfer.py
    "$CEEDLING_SCRIPT" check
    if find firmware/tests -type f -name 'test_*.c' -print -quit | grep -q .; then
        "$CEEDLING_SCRIPT" test:all
    else
        echo "No Ceedling unit tests found; configuration passed and test execution was skipped."
    fi
fi

if [ "$SKIP_FIRMWARE" -eq 0 ]; then
    if [ ! -f "$PICO_SDK_PATH_VALUE/external/pico_sdk_import.cmake" ]; then
        echo "Pico SDK is not available at $PICO_SDK_PATH_VALUE." >&2
        echo "Initialize submodules with 'git submodule update --init --recursive', or pass --pico-sdk-path." >&2
        exit 1
    fi

    build_firmware() {
        board_name="$1"
        set -- --board "$board_name" --pico-sdk-path "$PICO_SDK_PATH_VALUE"
        if [ -n "$GENERATOR" ]; then
            set -- "$@" --generator "$GENERATOR"
        fi
        "$FIRMWARE_BUILD_SCRIPT" "$@"
    }

    echo "=== Firmware builds ==="
    if [ -n "$BOARD" ]; then
        build_firmware "$BOARD"
    else
        build_firmware pico
        build_firmware pico2
    fi
fi