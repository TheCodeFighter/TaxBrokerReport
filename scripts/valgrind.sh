#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

# Unit test suites whose inputs are megabytes long. Valgrind runs them for minutes, so CI checks
# them in their own job, and only when a related file changes.
large_input_filter='*LargeInputTest.*'

usage() {
    cat <<'EOF'
Usage: valgrind.sh [--reuse-build] [--part regular|large|all]

  Builds build/dev from scratch, then runs each C++ executable under Valgrind
  inside the development container. The dump tools use synthetic test data.
  --reuse-build runs the existing build/dev as it is and compiles nothing.
  --part regular  every executable, without the *LargeInputTest unit test suites
  --part large    only the *LargeInputTest unit test suites (minutes under Valgrind)
  --part all      both (the default)
EOF
}

reuse_build=false
part=all

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        --reuse-build)
            reuse_build=true
            shift
            ;;
        --part)
            if [[ $# -lt 2 ]]; then
                echo "--part needs a value." >&2
                usage >&2
                exit 1
            fi

            case "$2" in
                regular | large | all)
                    part="$2"
                    ;;
                *)
                    echo "Unknown part: $2" >&2
                    usage >&2
                    exit 1
                    ;;
            esac

            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

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

if [[ "$part" == large ]]; then
    run_valgrind \
        /workspace/build/dev/tests/taxbroker_unit_tests \
        --gtest_brief=1 \
        --gtest_fail_if_no_test_selected \
        "--gtest_filter=$large_input_filter"
    exit 0
fi

if [[ "$part" == regular ]]; then
    run_valgrind \
        /workspace/build/dev/tests/taxbroker_unit_tests \
        --gtest_brief=1 \
        "--gtest_filter=-$large_input_filter"
else
    run_valgrind \
        /workspace/build/dev/tests/taxbroker_unit_tests \
        --gtest_brief=1
fi

run_valgrind \
    /workspace/build/dev/tests/taxbroker_integration_tests \
    --gtest_brief=1

run_valgrind /workspace/build/dev/src/taxbroker_server

run_valgrind \
    /workspace/build/dev/src/taxbroker \
    report \
    --year 2024 \
    --taxpayer /workspace/tests/test_data/cli/taxpayer.json \
    --out /tmp/taxbroker-cli-report \
    /workspace/tests/test_data/cli/report_fixture.csv

run_valgrind \
    /workspace/build/dev/tools/taxbroker_tr_dump \
    /workspace/tests/test_data/csv/traderepublic_parser_supported_fixture.csv \
    /tmp/taxbroker-tr-parsed.txt \
    /tmp/taxbroker-tr-diagnostics.json

run_valgrind \
    /workspace/build/dev/tools/taxbroker_exact_dump \
    /workspace/tests/test_data/csv/traderepublic_parser_supported_fixture.csv \
    /tmp/taxbroker-exact-arithmetic.txt
