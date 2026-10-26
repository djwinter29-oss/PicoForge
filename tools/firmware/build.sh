#!/usr/bin/env sh
set -eu

BOARD="${PICO_BOARD:-pico}"
BUILD_DIR=""
GENERATOR="${CMAKE_GENERATOR:-}"
PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-}"
FREERTOS_KERNEL_PATH_VALUE="${FREERTOS_KERNEL_PATH:-}"
SDCARD_TRANSPORT="${PF_SDCARD_TRANSPORT:-SDIO_PIO}"
FIRMWARE_VERSION="${PF_FIRMWARE_VERSION:-0.0.0-dev}"

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
        --freertos-kernel-path)
            FREERTOS_KERNEL_PATH_VALUE="$2"
            shift 2
            ;;
        --firmware-version)
            FIRMWARE_VERSION="$2"
            shift 2
            ;;
        --sdcard-transport)
            SDCARD_TRANSPORT="$2"
            shift 2
            ;;
        --help)
            cat <<'EOF'
Usage: tools/firmware/build.sh [options]

Options:
    --board NAME       Supported board: pico, pico_w, pico2 or pico2_w (default: pico)
    --sdcard-transport NAME  SDIO_PIO or SPI (default: SDIO_PIO)
    --build-dir PATH   Build directory (default: build/firmware-NAME-TRANSPORT)
    --generator NAME   CMake generator (default: Ninja when available)
    --pico-sdk-path PATH
    --freertos-kernel-path PATH  Override firmware/third_party/FreeRTOS-Kernel
    --firmware-version VERSION  Firmware version (default: PF_FIRMWARE_VERSION or 0.0.0-dev)
    This development build explicitly enables the storage benchmark (CMake default: OFF).
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
    PICO_SDK_PATH_VALUE="$SOURCE_DIR/third_party/pico-sdk"
fi
if [ ! -f "$PICO_SDK_PATH_VALUE/external/pico_sdk_import.cmake" ]; then
    echo "Pico SDK is not available at $PICO_SDK_PATH_VALUE." >&2
    echo "Initialize submodules with 'git submodule update --init --recursive', or pass --pico-sdk-path." >&2
    exit 1
fi
if [ -z "$FREERTOS_KERNEL_PATH_VALUE" ]; then
    FREERTOS_KERNEL_PATH_VALUE="$SOURCE_DIR/third_party/FreeRTOS-Kernel"
fi
if [ ! -f "$FREERTOS_KERNEL_PATH_VALUE/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake" ]; then
    echo "FreeRTOS-Kernel is not available at $FREERTOS_KERNEL_PATH_VALUE." >&2
    echo "Initialize the dependency with 'git submodule update --init --recursive', or pass --freertos-kernel-path." >&2
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
    BUILD_DIR="$REPO_ROOT/build/firmware-$BOARD-$SDCARD_TRANSPORT"
elif [ "${BUILD_DIR#/}" = "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPICO_SDK_PATH="$PICO_SDK_PATH_VALUE" \
    -DFREERTOS_KERNEL_PATH="$FREERTOS_KERNEL_PATH_VALUE" \
    -DPICO_BOARD="$BOARD" \
    -DPF_FIRMWARE_VERSION="$FIRMWARE_VERSION" \
    -DPF_ENABLE_STORAGE_BENCHMARK=ON \
    -DPF_SDCARD_PORT=FREERTOS \
    -DPF_SDCARD_TRANSPORT="$SDCARD_TRANSPORT"
cmake --build "$BUILD_DIR" --parallel ${CMAKE_BUILD_PARALLEL_LEVEL:+"$CMAKE_BUILD_PARALLEL_LEVEL"}
