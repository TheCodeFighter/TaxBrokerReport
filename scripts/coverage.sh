#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: coverage.sh

  Builds and runs all tests with GCC coverage instrumentation inside the
  development container. Combined reports are written to coverage/;
  unit-only and integration-only reports are written to its suite subdirectories.
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

if [[ "${TBR_COVERAGE_SKIP_IMAGE_BUILD:-0}" == "1" ]]; then
    ensure_dev_image
else
    echo "==> Ensuring the development image contains the coverage tools..."
    compose build dev
fi

echo "==> Configuring the isolated coverage build..."
compose run --rm -T -e CC=gcc -e CXX=g++ dev bash -s <<'CONTAINER_SCRIPT'
set -euo pipefail

coverage_build_dir=/workspace/build/coverage
coverage_report_dir=/workspace/coverage
lcov_options="--quiet --rc lcov_branch_coverage=1"

cmake \
    -S /workspace \
    -B "$coverage_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -fprofile-abs-path -Wno-missing-field-initializers" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage"

# Removed targets leave gcno files behind, and zeroing counters only removes gcda files.
# Rebuild objects and coverage metadata together so capture uses only the current source tree.
cmake --build "$coverage_build_dir" --target clean
find "$coverage_build_dir" -type f \( -name '*.gcno' -o -name '*.gcda' \) -delete

cmake --build "$coverage_build_dir" --parallel

lcov $lcov_options \
    --zerocounters \
    --directory "$coverage_build_dir"

cmake -E remove_directory "$coverage_report_dir"
cmake -E make_directory "$coverage_report_dir"

initial_capture_log=/tmp/taxbroker-lcov-initial.log
if ! lcov $lcov_options \
    --capture \
    --initial \
    --directory "$coverage_build_dir" \
    --output-file "$coverage_report_dir/initial.info" \
    >"$initial_capture_log" 2>&1; then
    cat "$initial_capture_log" >&2
    exit 1
fi

write_report() {
    local report_dir="$1"
    local report_title="$2"
    local lines_found lines_hit line_rate

    genhtml $lcov_options \
        "$report_dir/lcov.info" \
        --output-directory "$report_dir/html" \
        --title "$report_title" \
        --legend

    lcov --rc lcov_branch_coverage=1 \
        --summary "$report_dir/lcov.info" \
        >"$report_dir/summary.txt" 2>&1

    lines_found="$(awk -F: '$1 == "LF" { total += $2 } END { print total + 0 }' \
        "$report_dir/lcov.info")"
    lines_hit="$(awk -F: '$1 == "LH" { total += $2 } END { print total + 0 }' \
        "$report_dir/lcov.info")"

    if [ "$lines_found" -eq 0 ]; then
        echo "Coverage report contains no instrumented project lines: $report_title" >&2
        exit 1
    fi

    line_rate="$(awk -v hit="$lines_hit" -v found="$lines_found" \
        'BEGIN { printf "%.4f", (100 * hit) / found }')"

    {
        printf "lines_hit=%s\n" "$lines_hit"
        printf "lines_found=%s\n" "$lines_found"
        printf "line_rate=%s\n" "$line_rate"
    } >"$report_dir/metrics.env"

    cat "$report_dir/summary.txt"
    printf "%s: %s%% (%s of %s lines)\n" \
        "$report_title" "$line_rate" "$lines_hit" "$lines_found"
}

for suite in unit integration; do
    suite_report_dir="$coverage_report_dir/$suite"
    cmake -E make_directory "$suite_report_dir"

    lcov $lcov_options \
        --zerocounters \
        --directory "$coverage_build_dir"

    # Separate executables also work when measuring base commits without CTest labels.
    echo "==> Running $suite tests with coverage instrumentation..."
    "$coverage_build_dir/tests/taxbroker_${suite}_tests"

    lcov $lcov_options \
        --capture \
        --directory "$coverage_build_dir" \
        --output-file "$suite_report_dir/tests.info"

    # The shared zero-hit baseline keeps every suite's production-code denominator identical.
    lcov $lcov_options \
        --add-tracefile "$coverage_report_dir/initial.info" \
        --add-tracefile "$suite_report_dir/tests.info" \
        --output-file "$suite_report_dir/combined.info"

    lcov $lcov_options \
        --extract "$suite_report_dir/combined.info" \
        "/workspace/include/*" \
        "/workspace/src/*" \
        --output-file "$suite_report_dir/lcov.info"

    cmake -E rm -f "$suite_report_dir/tests.info" "$suite_report_dir/combined.info"
    write_report "$suite_report_dir" "TaxBrokerReport $suite coverage"
done

lcov $lcov_options \
    --add-tracefile "$coverage_report_dir/unit/lcov.info" \
    --add-tracefile "$coverage_report_dir/integration/lcov.info" \
    --output-file "$coverage_report_dir/lcov.info"

cmake -E rm -f "$coverage_report_dir/initial.info"
write_report "$coverage_report_dir" "TaxBrokerReport combined coverage"
CONTAINER_SCRIPT

echo "==> Combined HTML report: $repo_root/coverage/html/index.html"
echo "==> Unit HTML report: $repo_root/coverage/unit/html/index.html"
echo "==> Integration HTML report: $repo_root/coverage/integration/html/index.html"
