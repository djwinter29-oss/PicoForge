#!/usr/bin/env sh
# Strict release boundary; local builds retain version.cmake's prerelease support.
set -eu
tag=${1:-}
version=${tag#v}
if [ "$#" -ne 1 ] || [ "$(printf '%s\n' "$tag" | tr -d '\n')" != "$tag" ] || ! printf '%s\n' "$tag" | LC_ALL=C grep -Eq '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
    echo 'Release tag must be exactly vMAJOR.MINOR.PATCH (no leading zeros).' >&2
    exit 1
fi
script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cmake -DPF_FIRMWARE_VERSION="$version" -P "$script_dir/../../firmware/version.cmake" >&2
printf '%s\n' "$version"
