#!/usr/bin/env sh
set -eu

BOARD="${PICO_BOARD:-pico}"
BUILD_DIR=""
GENERATOR="${CMAKE_GENERATOR:-}"
PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-}"
FREERTOS_KERNEL_PATH_VALUE="${FREERTOS_KERNEL_PATH:-}"
SYS_CLK_MHZ="${PF_SYS_CLK_MHZ:-0}"
SD_CARD_TRANSPORT="${PF_SD_CARD_TRANSPORT:-SDIO_PIO}"
SD_CARD_PORT="${PF_SD_CARD_PORT:-FREERTOS}"
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
        --sys-clk-mhz)
            SYS_CLK_MHZ="$2"
            shift 2
            ;;
        --firmware-version)
            FIRMWARE_VERSION="$2"
            shift 2
            ;;
        --sd-card-transport)
            SD_CARD_TRANSPORT="$2"
            shift 2
            ;;
        --sd-card-port)
            SD_CARD_PORT="$2"
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
    --freertos-kernel-path PATH  Override firmware/third_party/FreeRTOS-Kernel
    --sys-clk-mhz MHZ  Out-of-spec system clock, 100-250 (default: 0, SDK default)
    --firmware-version VERSION  Firmware version (default: PF_FIRMWARE_VERSION or 0.0.0-dev)
    --sd-card-transport NAME  SDIO_PIO or SPI (default: SDIO_PIO)
    --sd-card-port NAME  FREERTOS application or BAREMETAL driver smoke (default: FREERTOS)
EOF
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

case "$SD_CARD_PORT" in
    FREERTOS|BAREMETAL) ;;
    *) echo "--sd-card-port must be FREERTOS or BAREMETAL" >&2; exit 2 ;;
esac

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
if [ "$SD_CARD_PORT" = FREERTOS ] &&
   [ ! -f "$FREERTOS_KERNEL_PATH_VALUE/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake" ]; then
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
    BUILD_DIR="$REPO_ROOT/build/firmware-$BOARD"
    if [ "$SD_CARD_PORT" = BAREMETAL ]; then
        BUILD_DIR="$BUILD_DIR-baremetal"
    fi
elif [ "${BUILD_DIR#/}" = "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPICO_SDK_PATH="$PICO_SDK_PATH_VALUE" \
    -DFREERTOS_KERNEL_PATH="$FREERTOS_KERNEL_PATH_VALUE" \
    -DPICO_BOARD="$BOARD" \
    -DPF_FIRMWARE_VERSION="$FIRMWARE_VERSION" \
    -DPF_SYS_CLK_MHZ="$SYS_CLK_MHZ" \
    -DPF_SD_CARD_PORT="$SD_CARD_PORT" \
    -DPF_SD_CARD_TRANSPORT="$SD_CARD_TRANSPORT"
cmake --build "$BUILD_DIR" --parallel