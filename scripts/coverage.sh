#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: coverage.sh

  Removes build/coverage, then builds and runs all tests from scratch with GCC
  coverage instrumentation inside the development container. Needs the dev
  image (scripts/build.sh image). Combined reports are written to coverage/;
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
ensure_dev_image

echo "==> Configuring the isolated coverage build from scratch..."
compose run --rm -T -e CC=gcc -e CXX=g++ dev bash -s <<'CONTAINER_SCRIPT'
set -euo pipefail

coverage_build_dir=/workspace/build/coverage
coverage_report_dir=/workspace/coverage
lcov_options="--quiet --rc lcov_branch_coverage=1"

# Start from an empty tree: old gcno and gcda files would otherwise leak into the capture.
# This stays inside the script because CI also runs it on base commits with their older lib.sh,
# so use only lib.sh functions that the base commit already has.
cmake -E rm -rf "$coverage_build_dir"

# FetchContent downloads the dependencies from GitHub during the configure, and CMake gives up at
# once on an HTTP error such as a 504, so the configure runs up to three times. The loop stays in
# this script (not lib.sh) for the base-commit runs described above.
for attempt in 1 2 3; do
    if cmake \
        -S /workspace \
        -B "$coverage_build_dir" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="--coverage -fprofile-abs-path" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage"; then
        break
    fi

    if [ "$attempt" -eq 3 ]; then
        echo "==> Configure failed 3 times." >&2
        exit 1
    fi

    wait_seconds=$((attempt == 1 ? 30 : 90))
    echo "==> Configure failed (attempt $attempt of 3), maybe a dependency download." \
        "Retrying in $wait_seconds s..." >&2
    sleep "$wait_seconds"
done

cmake --build "$coverage_build_dir" --parallel

lcov $lcov_options \
    --zerocounters \
    --directory "$coverage_build_dir"

cmake -E remove_directory "$coverage_report_dir"
cmake -E make_directory "$coverage_report_dir"

# lcov 1.16 runs gcov on one core, so the capture splits the coverage files into one list per core
# and merges the parts. The merged result is the same as one capture over the whole tree.
# Arguments: output tracefile, file extension (gcno for --initial, gcda after a test run), and
# the extra lcov options.
capture_parallel() {
    local output_file="$1"
    local extension="$2"
    shift 2
    local parts_dir=/tmp/taxbroker-lcov-parts
    local jobs index
    local -a pids add_options

    jobs="$(nproc)"
    rm -rf "$parts_dir"
    mkdir -p "$parts_dir"

    # Largest files first, dealt round-robin, so the lists get about the same work.
    find "$coverage_build_dir" -name "*.$extension" -exec stat -c '%s %n' {} + |
        sort -rn |
        awk -v jobs="$jobs" -v dir="$parts_dir" '{ print $2 > (dir "/list" (NR - 1) % jobs) }'

    for ((index = 0; index < jobs; index++)); do
        if [ ! -s "$parts_dir/list$index" ]; then
            continue
        fi

        # The intermediate (JSON) gcov format gives every lcov process its own temporary folder.
        # The older text format writes .gcov files into the object folder, where parallel captures
        # would overwrite each other, so it is turned off: without JSON support gcov fails loudly.
        # shellcheck disable=SC2046
        lcov $lcov_options \
            --rc geninfo_intermediate=1 \
            --capture \
            "$@" \
            $(sed 's/^/--directory /' "$parts_dir/list$index") \
            --output-file "$parts_dir/part$index.info" \
            >"$parts_dir/log$index" 2>&1 &
        pids[index]=$!
    done

    # A failed part prints its log and stops; a part that worked still prints its warnings.
    for index in "${!pids[@]}"; do
        if ! wait "${pids[index]}"; then
            cat "$parts_dir/log$index" >&2
            exit 1
        fi

        if [ -s "$parts_dir/log$index" ]; then
            cat "$parts_dir/log$index" >&2
        fi
    done

    for index in "${!pids[@]}"; do
        add_options+=(--add-tracefile "$parts_dir/part$index.info")
    done

    lcov $lcov_options "${add_options[@]}" --output-file "$output_file"
    rm -rf "$parts_dir"
}

capture_parallel "$coverage_report_dir/initial.info" gcno --initial

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

    capture_parallel "$suite_report_dir/tests.info" gcda

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
