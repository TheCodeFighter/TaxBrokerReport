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
echo "The commit message is checked on commit; formatting is checked on commit and push, for the changed files only."
echo "CI checks all files. Run scripts/format.sh to format locally."
