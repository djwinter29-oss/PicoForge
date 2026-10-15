#!/usr/bin/env sh
set -eu

BOARD="${PICO_BOARD:-pico}"
BUILD_DIR=""
ELF_PATH=""
OPENOCD_EXE="${OPENOCD_EXE:-openocd}"
OPENOCD_TARGET="${PICO_OPENOCD_TARGET:-}"
ADAPTER_SPEED_KHZ="${PICO_DEBUG_PROBE_SPEED_KHZ:-5000}"
DEBUG_PROBE_SERIAL="${PICO_DEBUG_PROBE_SERIAL:-}"
GENERATOR="${CMAKE_GENERATOR:-}"
PICO_SDK_PATH_VALUE="${PICO_SDK_PATH:-}"
FREERTOS_KERNEL_PATH_VALUE="${FREERTOS_KERNEL_PATH:-}"
SD_CARD_TRANSPORT="${PF_SD_CARD_TRANSPORT:-SDIO_PIO}"
SKIP_BUILD=0

usage() {
    cat <<'EOF'
Usage: tools/firmware/load.sh [options]

Build and flash PicoForge to a Pico using a CMSIS-DAP debug probe.

Options:
--board NAME                 Pico SDK board (default: pico)
--build-dir PATH             Build directory (default: build/firmware-NAME)
--elf PATH                   ELF to flash (default: pf_hello_world.elf in build dir)
--openocd-exe PATH           OpenOCD executable (default: openocd)
--openocd-target PATH        OpenOCD target config (default: inferred from board)
--adapter-speed-khz KHZ      Debug probe speed (default: 5000)
--probe-serial SERIAL        Select a CMSIS-DAP probe by serial number
--generator NAME             CMake generator
--pico-sdk-path PATH         Override Pico SDK path
--freertos-kernel-path PATH  Override FreeRTOS-Kernel path
--sd-card-transport NAME     SDIO_PIO or SPI (default: SDIO_PIO)
--skip-build                 Flash an existing ELF without building
--help                       Show this help
EOF
}

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
        --elf)
            ELF_PATH="$2"
            shift 2
            ;;
        --openocd-exe)
            OPENOCD_EXE="$2"
            shift 2
            ;;
        --openocd-target)
            OPENOCD_TARGET="$2"
            shift 2
            ;;
        --adapter-speed-khz)
            ADAPTER_SPEED_KHZ="$2"
            shift 2
            ;;
        --probe-serial)
            DEBUG_PROBE_SERIAL="$2"
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
        --sd-card-transport)
            SD_CARD_TRANSPORT="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --help|-h)
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

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/build/firmware-$BOARD"
elif [ "${BUILD_DIR#/}" = "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
fi

if [ "$SKIP_BUILD" -eq 0 ]; then
    set -- "$SCRIPT_DIR/build.sh" --board "$BOARD" --build-dir "$BUILD_DIR"
    if [ -n "$GENERATOR" ]; then
        set -- "$@" --generator "$GENERATOR"
    fi
    if [ -n "$PICO_SDK_PATH_VALUE" ]; then
        set -- "$@" --pico-sdk-path "$PICO_SDK_PATH_VALUE"
    fi
    if [ -n "$FREERTOS_KERNEL_PATH_VALUE" ]; then
        set -- "$@" --freertos-kernel-path "$FREERTOS_KERNEL_PATH_VALUE"
    fi
    set -- "$@" --sd-card-transport "$SD_CARD_TRANSPORT" --sd-card-port FREERTOS
    "$@"
fi

if [ -z "$ELF_PATH" ]; then
    if [ -f "$BUILD_DIR/CMakeCache.txt" ] &&
       grep -q '^PF_SD_CARD_PORT:STRING=BAREMETAL$' "$BUILD_DIR/CMakeCache.txt"; then
        echo "The selected build uses the bare-metal driver smoke target, not pf_hello_world." >&2
        echo "Select a FreeRTOS application build directory or an explicit --elf." >&2
        exit 1
    fi
    ELF_PATH="$BUILD_DIR/pf_hello_world.elf"
elif [ "${ELF_PATH#/}" = "$ELF_PATH" ]; then
    ELF_PATH="$REPO_ROOT/$ELF_PATH"
fi

if [ ! -f "$ELF_PATH" ]; then
    echo "ELF file not found: $ELF_PATH" >&2
    exit 1
fi

if [ -z "$OPENOCD_TARGET" ]; then
    case "$BOARD" in
        pico|pico_w|pico_h|pico_wh|rp2040*)
            OPENOCD_TARGET="target/rp2040.cfg"
            ;;
        pico2|pico2_w|pico2*)
            OPENOCD_TARGET="target/rp2350.cfg"
            ;;
        *)
            echo "No default OpenOCD target for board '$BOARD'. Use --openocd-target." >&2
            exit 1
            ;;
    esac
fi

if ! command -v "$OPENOCD_EXE" >/dev/null 2>&1; then
    echo "OpenOCD executable not found: $OPENOCD_EXE" >&2
    exit 1
fi

tcl_escape() {
    printf '%s' "$1" | sed 's/[{}]/\\&/g'
}

ELF_PATH_TCL=$(tcl_escape "$ELF_PATH")
set -- "$OPENOCD_EXE" -f interface/cmsis-dap.cfg

if [ -n "$DEBUG_PROBE_SERIAL" ]; then
    DEBUG_PROBE_SERIAL_TCL=$(tcl_escape "$DEBUG_PROBE_SERIAL")
    set -- "$@" -c "adapter serial {$DEBUG_PROBE_SERIAL_TCL}"
fi

set -- "$@" \
    -f "$OPENOCD_TARGET" \
    -c "adapter speed $ADAPTER_SPEED_KHZ" \
    -c "program {$ELF_PATH_TCL} verify reset exit"

"$@"