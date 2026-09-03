#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: valgrind.sh

  Builds the normal Debug test binaries when needed, then runs each test
  executable once under Valgrind inside the development container.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

if [[ $# -eq 1 ]]; then
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
fi

ensure_build_tree_writable

if [[ ! -f "$repo_root/build/build.ninja" ]]; then
    echo "==> Build tree not found; building the backend first..."
    "$script_dir/build.sh" dev
else
    ensure_dev_image
    echo "==> Refreshing the backend build..."
    compose run --rm -T dev cmake --build /workspace/build --parallel
fi

ensure_build_outputs_executable

test_binaries=(
    "/workspace/build/tests/taxbroker_unit_tests"
    "/workspace/build/tests/taxbroker_integration_tests"
)

for test_binary in "${test_binaries[@]}"; do
    echo "==> Running $(basename "$test_binary") under Valgrind..."
    compose run --rm -T dev valgrind \
        --leak-check=full \
        --show-leak-kinds=definite,indirect \
        --errors-for-leak-kinds=definite,indirect \
        --track-origins=yes \
        --error-exitcode=1 \
        "$test_binary" \
        --gtest_brief=1
done
