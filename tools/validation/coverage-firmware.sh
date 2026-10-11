#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
FIRMWARE_TEST_DIR="$REPO_ROOT/firmware/tests"

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    cat <<'EOF'
Usage: tools/validation/coverage-firmware.sh [Ceedling options]

Runs Ceedling's gcov:all task for host-testable firmware modules.
HTML and Cobertura reports are written under build/ceedling/artifacts/gcov/gcovr/.
At least one firmware/tests/test_*.c file is required.
EOF
    exit 0
fi

if ! find "$FIRMWARE_TEST_DIR" -type f -name 'test_*.c' -print -quit | grep -q .; then
    echo "No Ceedling tests found under $FIRMWARE_TEST_DIR." >&2
    echo "Add a host-testable module and its test_*.c file before generating coverage." >&2
    exit 1
fi

exec "$REPO_ROOT/tools/validation/setup-ceedling-testing.sh" gcov:all "$@"