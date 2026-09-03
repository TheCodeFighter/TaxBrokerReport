#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: valgrind.sh

  Builds every C++ executable, then runs each one under Valgrind inside the
  development container. The Trade Republic dump tool uses synthetic test data.
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
    /workspace/build/tests/taxbroker_unit_tests \
    --gtest_brief=1

run_valgrind \
    /workspace/build/tests/taxbroker_integration_tests \
    --gtest_brief=1

run_valgrind /workspace/build/src/taxbroker_server

debug_tools_build_dir="/workspace/build/debug-tools"

echo "==> Configuring the isolated debug tools build..."
compose run --rm -T \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_tools_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Trade Republic dump tool..."
compose run --rm -T \
    dev cmake --build "$debug_tools_build_dir" --target taxbroker_tr_dump --parallel

run_valgrind \
    "$debug_tools_build_dir/tools/taxbroker_tr_dump" \
    /workspace/tests/test_data/csv/traderepublic_parser_supported_fixture.csv \
    /tmp/taxbroker-tr-parsed.txt \
    /tmp/taxbroker-tr-diagnostics.json
