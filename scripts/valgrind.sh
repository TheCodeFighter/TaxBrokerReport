#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: valgrind.sh [--reuse-build]

  Builds build/dev from scratch, then runs each C++ executable under Valgrind
  inside the development container. The dump tools use synthetic test data.
  --reuse-build runs the existing build/dev as it is and compiles nothing.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

reuse_build=false

if [[ $# -eq 1 ]]; then
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        --reuse-build)
            reuse_build=true
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
fi

prepare_dev_build "$reuse_build"

run_valgrind() {
    local executable="$1"
    shift

    echo "==> Running $(basename "$executable") under Valgrind..."
    compose run --rm -T \
        -e TBR_LOG_FILE=/tmp/taxbroker-valgrind.log \
        dev valgrind \
        --leak-check=full \
        --show-leak-kinds=definite,indirect \
        --errors-for-leak-kinds=definite,indirect \
        --track-origins=yes \
        --error-exitcode=1 \
        "$executable" \
        "$@"
}

run_valgrind \
    /workspace/build/dev/tests/taxbroker_unit_tests \
    --gtest_brief=1

run_valgrind \
    /workspace/build/dev/tests/taxbroker_integration_tests \
    --gtest_brief=1

run_valgrind /workspace/build/dev/src/taxbroker_server

run_valgrind \
    /workspace/build/dev/tools/taxbroker_tr_dump \
    /workspace/tests/test_data/csv/traderepublic_parser_supported_fixture.csv \
    /tmp/taxbroker-tr-parsed.txt \
    /tmp/taxbroker-tr-diagnostics.json

run_valgrind \
    /workspace/build/dev/tools/taxbroker_exact_dump \
    /workspace/tests/test_data/csv/traderepublic_parser_supported_fixture.csv \
    /tmp/taxbroker-exact-arithmetic.txt
