#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: coverage.sh

  Builds and runs all tests with GCC coverage instrumentation inside the
  development container. Reports are written to coverage/.
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

# CMake's GoogleTest discovery executes the rebuilt test binary during the build. Clear counters
# from the previous binary first so libgcov never sees stale checksums during that discovery run.
if [ -d "$coverage_build_dir" ]; then
    lcov $lcov_options \
        --zerocounters \
        --directory "$coverage_build_dir"
fi

cmake \
    -S /workspace \
    -B "$coverage_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -fprofile-abs-path -Wno-missing-field-initializers" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage"

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

echo "==> Running all tests with coverage instrumentation..."
ctest --test-dir "$coverage_build_dir" --output-on-failure

lcov $lcov_options \
    --capture \
    --directory "$coverage_build_dir" \
    --output-file "$coverage_report_dir/tests.info"

lcov $lcov_options \
    --add-tracefile "$coverage_report_dir/initial.info" \
    --add-tracefile "$coverage_report_dir/tests.info" \
    --output-file "$coverage_report_dir/combined.info"

lcov $lcov_options \
    --extract "$coverage_report_dir/combined.info" \
    "/workspace/include/*" \
    "/workspace/src/*" \
    --output-file "$coverage_report_dir/lcov.info"

cmake -E rm -f \
    "$coverage_report_dir/initial.info" \
    "$coverage_report_dir/tests.info" \
    "$coverage_report_dir/combined.info"

genhtml $lcov_options \
    "$coverage_report_dir/lcov.info" \
    --output-directory "$coverage_report_dir/html" \
    --title "TaxBrokerReport coverage" \
    --legend

lcov --rc lcov_branch_coverage=1 \
    --summary "$coverage_report_dir/lcov.info" \
    >"$coverage_report_dir/summary.txt" 2>&1

lines_found="$(awk -F: '$1 == "LF" { total += $2 } END { print total + 0 }' \
    "$coverage_report_dir/lcov.info")"
lines_hit="$(awk -F: '$1 == "LH" { total += $2 } END { print total + 0 }' \
    "$coverage_report_dir/lcov.info")"

if [ "$lines_found" -eq 0 ]; then
    echo "Coverage report contains no instrumented project lines." >&2
    exit 1
fi

line_rate="$(awk -v hit="$lines_hit" -v found="$lines_found" \
    'BEGIN { printf "%.4f", (100 * hit) / found }')"

{
    printf "lines_hit=%s\n" "$lines_hit"
    printf "lines_found=%s\n" "$lines_found"
    printf "line_rate=%s\n" "$line_rate"
} >"$coverage_report_dir/metrics.env"

cat "$coverage_report_dir/summary.txt"
printf "Exact line coverage: %s%% (%s of %s lines)\n" \
    "$line_rate" \
    "$lines_hit" \
    "$lines_found"
CONTAINER_SCRIPT

echo "==> HTML report: $repo_root/coverage/html/index.html"
