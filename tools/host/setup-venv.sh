#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
REQUIREMENTS_FILE="$SCRIPT_DIR/requirements-coverage.txt"
VENV_DIR="${VENV_DIR:-$REPO_ROOT/build/coverage-tools}"
PYTHON="${PYTHON:-python3}"
GCOVR_VERSION="8.6"

usage() {
	cat <<'EOF'
Usage: tools/host/setup-venv.sh [--venv-dir PATH]

Create the Python environment for PicoForge's Ceedling coverage tools.
Relative --venv-dir paths are resolved from the current working directory.
Set PYTHON to select the Python 3.10+ interpreter.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--venv-dir)
			if [[ $# -lt 2 ]]; then
				usage >&2
				exit 2
			fi
			VENV_DIR="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			printf 'Unknown argument: %s\n' "$1" >&2
			usage >&2
			exit 2
			;;
	esac
done

if [[ ! -f "$REQUIREMENTS_FILE" ]]; then
	echo "Coverage requirements file not found: $REQUIREMENTS_FILE" >&2
	exit 1
fi
if ! "$PYTHON" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))'; then
	printf 'Coverage tools require Python 3.10 or newer (%s)\n' "$PYTHON" >&2
	exit 1
fi

if [[ "$VENV_DIR" == "~" ]]; then
	VENV_DIR="$HOME"
elif [[ "$VENV_DIR" == "~/"* ]]; then
	VENV_DIR="$HOME/${VENV_DIR#~/}"
fi
if [[ "$VENV_DIR" != /* ]]; then
	VENV_DIR="$PWD/$VENV_DIR"
fi
mkdir -p -- "$(dirname -- "$VENV_DIR")"
VENV_DIR="$(cd -- "$(dirname -- "$VENV_DIR")" && pwd)/$(basename -- "$VENV_DIR")"

VENV_PYTHON="$VENV_DIR/bin/python"
GCOVR_BIN="$VENV_DIR/bin/gcovr"
GCOVR_INSTALLED_VERSION=""
if [[ -x "$GCOVR_BIN" ]]; then
	GCOVR_INSTALLED_VERSION=$("$GCOVR_BIN" --version 2>/dev/null | sed -n '1p' || true)
fi
if [[ "$GCOVR_INSTALLED_VERSION" != "gcovr $GCOVR_VERSION" ]]; then
	if command -v uv >/dev/null 2>&1; then
		uv venv --clear --python "$PYTHON" "$VENV_DIR"
	else
		"$PYTHON" -m venv --clear "$VENV_DIR"
	fi
fi

if command -v uv >/dev/null 2>&1; then
	uv pip install --python "$VENV_PYTHON" -r "$REQUIREMENTS_FILE"
else
	"$VENV_PYTHON" -m pip install --disable-pip-version-check --no-input -r "$REQUIREMENTS_FILE"
fi

if [[ ! -x "$GCOVR_BIN" ]] || [[ "$("$GCOVR_BIN" --version | sed -n '1p')" != "gcovr $GCOVR_VERSION" ]]; then
	echo "gcovr $GCOVR_VERSION was not installed in $VENV_DIR." >&2
	exit 1
fi

printf 'Environment ready: %s\n' "$VENV_DIR"
printf 'Activate with: source %s/bin/activate\n' "$VENV_DIR"
printf 'Check gcovr with: %s --version\n' "$GCOVR_BIN"