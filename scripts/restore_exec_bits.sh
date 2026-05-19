#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

paths=(
    "scripts/build.sh"
    "scripts/format.sh"
    "scripts/install_hooks.sh"
    "scripts/run.sh"
    "scripts/test.sh"
    "scripts/restore_exec_bits.sh"
    ".githooks/commit-msg"
    ".githooks/pre-commit"
    ".githooks/pre-push"
)

for path in "${paths[@]}"; do
    if [[ -e "$repo_root/$path" && ! -x "$repo_root/$path" ]]; then
        chmod u+x "$repo_root/$path"
    fi
done
