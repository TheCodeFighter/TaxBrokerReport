#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

# Unit test suites whose inputs are megabytes long. Valgrind runs them for minutes, so CI checks
# each part in its own job, and only when a related file changes. A new large test goes into a
# *LargeInputTest suite of one part; the regular part leaves out every *LargeInputTest suite.
large_input_filter='*LargeInputTest.*'
large_parts=(csv ecb reference messages)

large_suite_of_part() {
    case "$1" in
        csv) echo 'CsvTextReaderLargeInputTest.*' ;;
        ecb) echo 'EcbRateFileLargeInputTest.*' ;;
        reference) echo 'ReferenceDataLargeInputTest.*' ;;
        messages) echo 'DiagnosticMessagesLargeInputTest.*' ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: valgrind.sh [--reuse-build] [--part regular|csv|ecb|reference|messages|all]

  Builds build/dev from scratch, then runs each C++ executable under Valgrind
  inside the development container. The dump tools use synthetic test data.
  --reuse-build runs the existing build/dev as it is and compiles nothing.
  --part regular    every executable, without the *LargeInputTest unit test suites, and the
                    out-of-memory tests in a short extra unit test run (see below)
  --part csv        the large-input suite of the CSV reader (minutes under Valgrind)
  --part ecb        the large-input suite of the ECB rate files
  --part reference  the large-input suite of the reference files
  --part messages   the large-input suite of the diagnostic message table
  --part all        regular and every large-input part (the default)
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
                regular | csv | ecb | reference | messages | all)
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

# Extra Valgrind options of the next run_valgrind call (a list, empty by default).
valgrind_options=()

run_valgrind() {
    local executable="$1"
    shift

    echo "==> Running $(basename "$executable") under Valgrind..."
    compose run --rm -T \
        -e TBR_LOG_FILE=/tmp/taxbroker-valgrind.log \
        -e TBR_REQUIRE_ALLOCATION_HOOK="${TBR_REQUIRE_ALLOCATION_HOOK:-}" \
        dev valgrind \
        "${valgrind_options[@]}" \
        --leak-check=full \
        --show-leak-kinds=definite,indirect \
        --errors-for-leak-kinds=definite,indirect \
        --track-origins=yes \
        --error-exitcode=1 \
        "$executable" \
        "$@"
}

run_large_part() {
    run_valgrind \
        /workspace/build/dev/tests/taxbroker_unit_tests \
        --gtest_brief=1 \
        --gtest_fail_if_no_test_selected \
        "--gtest_filter=$(large_suite_of_part "$1")"
}

# Valgrind replaces `operator new` of the test binary, so the allocation hook of the out-of-memory
# tests (tests/support/large_allocation_hook.hpp) never runs and those tests skip in the main
# unit test run. This run starts the same binary with the flag that leaves its own `operator new`
# alone, runs only those tests (their names have RunningOutOfMemory or AllocationHook in them) and
# fails when one of them skips. Leaks and invalid memory access are still checked.
run_allocation_hook_tests() {
    valgrind_options=(--soname-synonyms=somalloc=nouserintercepts)
    TBR_REQUIRE_ALLOCATION_HOOK=1 run_valgrind \
        /workspace/build/dev/tests/taxbroker_unit_tests \
        --gtest_brief=1 \
        --gtest_fail_if_no_test_selected \
        '--gtest_filter=*RunningOutOfMemory*:*AllocationHook*'
    valgrind_options=()
}

if [[ "$part" != regular && "$part" != all ]]; then
    run_large_part "$part"
    exit 0
fi

run_valgrind \
    /workspace/build/dev/tests/taxbroker_unit_tests \
    --gtest_brief=1 \
    "--gtest_filter=-$large_input_filter"

run_allocation_hook_tests

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
    --trade-republic \
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

if [[ "$part" == all ]]; then
    for large_part in "${large_parts[@]}"; do
        run_large_part "$large_part"
    done
fi
