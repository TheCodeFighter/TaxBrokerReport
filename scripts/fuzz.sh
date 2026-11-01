#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF_USAGE'
Usage: fuzz.sh [--seconds N] [--replay FILE]

  Builds the libFuzzer target of the Trade Republic parser with AddressSanitizer and
  UndefinedBehaviorSanitizer in build/fuzz (from scratch, with Clang) and runs it inside the
  development container for N seconds (default 60) with the fixed libFuzzer seed 1.
  The seeds are the synthetic CSV files under tests/test_data and tests/fuzz/seeds. New inputs go to
  runtime/fuzz/corpus, and an input that breaks a check is saved as runtime/fuzz/crash-<hash>.
  --replay runs one saved input again and runs nothing else. It takes a repository-relative path,
  an absolute host path or the /workspace path that libFuzzer prints.
EOF_USAGE
}

seconds=60
replay_file=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --seconds)
            if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]*$ ]]; then
                echo "--seconds requires a positive whole number." >&2
                usage >&2
                exit 1
            fi

            seconds="$2"
            shift 2
            ;;
        --replay)
            if [[ $# -lt 2 ]]; then
                echo "--replay requires an existing file of this repository." >&2
                usage >&2
                exit 1
            fi

            # Also accept the container path that libFuzzer prints and an absolute host path.
            replay_file="${2#/workspace/}"
            replay_file="${replay_file#"$repo_root"/}"

            if [[ ! -f "$repo_root/$replay_file" ]]; then
                echo "--replay requires an existing file of this repository." >&2
                usage >&2
                exit 1
            fi

            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

ensure_dev_image
ensure_build_tree_writable
reset_build_dir fuzz

echo "==> Configuring the fuzz build inside the dev container..."
retry_configure compose run --rm -T -e CC=clang -e CXX=clang++ dev cmake \
    -S /workspace \
    -B /workspace/build/fuzz \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-O1 -g" \
    -DTAXBROKER_BUILD_FUZZERS=ON

echo "==> Building the fuzz target..."
compose run --rm -T -e CC=clang -e CXX=clang++ dev \
    cmake --build /workspace/build/fuzz --parallel --target traderepublic_parser_fuzz

fuzzer=/workspace/build/fuzz/tests/fuzz/traderepublic_parser_fuzz

# The leak check of AddressSanitizer reports a leak at exit even for an empty program on Alpine
# (musl). Valgrind covers memory leaks.
if [[ -n "$replay_file" ]]; then
    echo "==> Replaying $replay_file..."
    compose run --rm -T -e ASAN_OPTIONS=detect_leaks=0 dev "$fuzzer" "/workspace/$replay_file"
    exit 0
fi

echo "==> Fuzzing for $seconds seconds (seed 1)..."
mkdir -p "$repo_root/runtime/fuzz"
marker="$(mktemp)"
trap 'rm -f "$marker"' EXIT

fuzz_status=0
compose run --rm -T -e ASAN_OPTIONS=detect_leaks=0 dev sh -c '
    set -eu
    rm -rf /workspace/runtime/fuzz/seeds
    mkdir -p /workspace/runtime/fuzz/corpus /workspace/runtime/fuzz/seeds
    # Sorted, so every machine numbers the seeds the same way and seed 1 gives the same run.
    n=0
    find /workspace/tests/test_data /workspace/tests/fuzz/seeds -name "*.csv" | LC_ALL=C sort |
        while IFS= read -r seed; do
            n=$((n + 1))
            cp "$seed" "/workspace/runtime/fuzz/seeds/seed-$n.csv"
        done
    "$1" \
        -seed=1 \
        -max_total_time="$2" \
        -max_len=8192 \
        -timeout=10 \
        -dict=/workspace/tests/fuzz/traderepublic.dict \
        -artifact_prefix=/workspace/runtime/fuzz/ \
        /workspace/runtime/fuzz/corpus \
        /workspace/runtime/fuzz/seeds
' sh "$fuzzer" "$seconds" || fuzz_status=$?

if [[ "$fuzz_status" -ne 0 ]]; then
    echo "==> The fuzzer stopped with exit code $fuzz_status." >&2

    # Only the files this run wrote: older findings stay in the folder.
    find "$repo_root/runtime/fuzz" -maxdepth 1 -type f -newer "$marker" \
        \( -name 'crash-*' -o -name 'leak-*' -o -name 'oom-*' -o -name 'slow-unit-*' \
        -o -name 'timeout-*' \) | LC_ALL=C sort |
        while IFS= read -r finding; do
            echo "Replay with: scripts/fuzz.sh --replay runtime/fuzz/$(basename "$finding")" >&2
        done

    exit "$fuzz_status"
fi

echo "==> No finding in $seconds seconds."
