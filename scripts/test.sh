#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

usage() {
    cat <<'EOF'
Usage: test.sh [ctest-regex]

  With no arguments, runs the full test suite.
  With one argument, runs only tests matching that CTest regular expression.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

test_filter="${1:-}"

if [[ "$test_filter" == "-h" || "$test_filter" == "--help" ]]; then
    usage
    exit 0
fi

ensure_build_tree_writable

if [[ ! -f "$repo_root/build/build.ninja" ]]; then
    echo "==> Build tree not found; building the dev image and backend first..."
    "$script_dir/build.sh" dev
else
    ensure_dev_image
    echo "==> Refreshing the backend build..."
    compose run --rm dev sh -lc 'cmake --build /workspace/build --parallel'
fi

if [[ -n "$test_filter" ]]; then
    echo "==> Running tests matching: $test_filter"
    compose run --rm dev ctest --test-dir /workspace/build --output-on-failure -R "$test_filter"
else
    echo "==> Running all tests..."
    compose run --rm dev ctest --test-dir /workspace/build --output-on-failure
fi
