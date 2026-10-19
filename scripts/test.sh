#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: test.sh [--reuse-build] [--suite unit|integration] [ctest-regex]

  With no arguments, builds build/dev from scratch and runs the full test suite.
  --reuse-build runs the existing build/dev as it is and compiles nothing.
  --suite selects tests by their CTest label.
  An optional regular expression further filters tests by name.
EOF
}

reuse_build=false
test_suite=""

if [[ "${1:-}" == "--reuse-build" ]]; then
    reuse_build=true
    shift
fi

if [[ "${1:-}" == "--suite" ]]; then
    if [[ $# -lt 2 || ( "$2" != "unit" && "$2" != "integration" ) ]]; then
        echo "--suite requires unit or integration." >&2
        usage >&2
        exit 1
    fi

    test_suite="$2"
    shift 2
fi

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

if [[ "$test_filter" == --* ]]; then
    echo "Unknown option: $test_filter" >&2
    usage >&2
    exit 1
fi

prepare_dev_build "$reuse_build"

ctest_options=(--test-dir /workspace/build/dev --output-on-failure --no-tests=error)

if [[ -n "$test_suite" ]]; then
    echo "==> Selected test suite: $test_suite"
    ctest_options+=(-L "^${test_suite}$")
fi

if [[ -n "$test_filter" ]]; then
    echo "==> Selected test-name filter: $test_filter"
    ctest_options+=(-R "$test_filter")
fi

echo "==> Running tests..."
compose run --rm dev ctest "${ctest_options[@]}"
