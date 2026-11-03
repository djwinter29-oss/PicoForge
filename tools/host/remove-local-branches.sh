#!/usr/bin/env sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

case "${1:-}" in
    -h|--help)
        cat <<'EOF'
Usage: tools/host/remove-local-branches.sh

Switch to main and force-delete every other local branch.
This also deletes unmerged commits on those branches. The script asks for
confirmation before making any changes.
EOF
        exit 0
        ;;
    "")
        ;;
    *)
        echo "Unknown argument: $1; use --help." >&2
        exit 2
        ;;
esac

cd "$REPO_ROOT"

if ! git show-ref --verify --quiet refs/heads/main; then
    echo "Local branch 'main' does not exist." >&2
    exit 1
fi

branch_list=$(git for-each-ref --format='%(refname:short)' refs/heads) || exit 1
other_branches=
# Newline-only splitting keeps spaces in branch names. Globbing stays off so a name
# containing * or ? is not expanded. git's failure above is not hidden by a pipeline.
set -f
old_ifs=$IFS
IFS='
'
for branch in $branch_list; do
    if [ -n "$branch" ] && [ "$branch" != main ]; then
        other_branches="${other_branches}${branch}
"
    fi
done
IFS=$old_ifs
set +f

if [ -z "$other_branches" ]; then
    git switch main
    echo "No other local branches to delete."
    exit 0
fi

printf 'The following local branches will be force-deleted:\n%s' "$other_branches"
printf 'This includes unmerged commits. Type "yes" to continue: '
IFS= read -r confirmation || confirmation=
if [ "$confirmation" != yes ]; then
    echo "Cancelled; no branches were deleted."
    exit 1
fi

git switch main
set -f
old_ifs=$IFS
IFS='
'
for branch in $other_branches; do
    if [ -n "$branch" ]; then
        git branch -D -- "$branch"
    fi
done
IFS=$old_ifs
set +f