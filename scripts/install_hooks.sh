#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
hooks_dir="$repo_root/.githooks"

bash "$script_dir/restore_exec_bits.sh"

git config core.hooksPath "$hooks_dir"

echo "Git hooks installed from: $hooks_dir"
echo "Formatting will now be checked on commit and push."