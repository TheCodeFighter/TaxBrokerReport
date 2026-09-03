#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
hooks_dir="$repo_root/.githooks"

chmod u+x \
    "$hooks_dir/commit-msg" \
    "$hooks_dir/pre-commit" \
    "$hooks_dir/pre-push"

git config core.hooksPath "$hooks_dir"

echo "Git hooks installed from: $hooks_dir"
echo "Formatting will now be checked on commit and push."
