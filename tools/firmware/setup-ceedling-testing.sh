#!/usr/bin/env sh
set -eu

CEEDLING_VERSION="1.1.10"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
PROJECT_FILE="$REPO_ROOT/project.yml"
COVERAGE_VENV="$REPO_ROOT/build/coverage-tools"

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    cat <<'EOF'
Usage: tools/firmware/setup-ceedling-testing.sh [Ceedling task and options]

Installs pinned Ceedling and gcovr, then runs Ceedling from the repository root.
Requires Ruby 3+, GCC/gcov, and Python 3.10+ with venv support or uv.
With no arguments, shows Ceedling help. For example:
tools/firmware/setup-ceedling-testing.sh test:all
EOF
    exit 0
fi

if [ ! -f "$PROJECT_FILE" ]; then
    echo "Ceedling project file not found: $PROJECT_FILE" >&2
    exit 1
fi
if ! command -v gcc >/dev/null 2>&1 || ! command -v gcov >/dev/null 2>&1; then
    echo "GCC and gcov are required for Ceedling coverage. Install the GCC toolchain with your OS package manager." >&2
    exit 1
fi
GCC_VERSION=$(gcc -dumpfullversion -dumpversion)
GCOV_VERSION_LINE=$(gcov --version | sed -n '1p')
case "$GCOV_VERSION_LINE" in
    *"$GCC_VERSION"*) ;;
    *)
        echo "gcov version must match gcc $GCC_VERSION; found: $GCOV_VERSION_LINE" >&2
        exit 1
        ;;
esac
if ! command -v ruby >/dev/null 2>&1 || ! command -v gem >/dev/null 2>&1; then
    echo "Ruby 3 or newer and RubyGems are required. Install them with your OS package manager." >&2
    exit 1
fi
if ! ruby -rrubygems -e 'exit(Gem::Version.new(RUBY_VERSION) >= Gem::Version.new("3.0") ? 0 : 1)'; then
    echo "Ruby 3 or newer is required for Ceedling $CEEDLING_VERSION." >&2
    exit 1
fi

GEM_BIN="$(gem env user_gemhome)/bin"
CEEDLING_BIN="$GEM_BIN/ceedling"
if [ ! -x "$CEEDLING_BIN" ]; then
    gem install ceedling --version "$CEEDLING_VERSION" --user-install --no-document
fi
if [ ! -x "$CEEDLING_BIN" ]; then
    echo "Ceedling $CEEDLING_VERSION was not installed at $CEEDLING_BIN." >&2
    exit 1
fi

"$REPO_ROOT/tools/host/setup-venv.sh" --venv-dir "$COVERAGE_VENV"
export PATH="$COVERAGE_VENV/bin:$PATH"
cd "$REPO_ROOT"
if [ "$#" -eq 0 ]; then
    set -- help
fi
exec "$CEEDLING_BIN" "$@"