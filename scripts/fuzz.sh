#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF_USAGE'
Usage: fuzz.sh [--target parser|multipart|answers] [--seconds N] [--replay FILE]

  Builds a libFuzzer target with AddressSanitizer and UndefinedBehaviorSanitizer in build/fuzz
  (from scratch, with Clang) and runs it inside the development container for N seconds (default
  60) with the fixed libFuzzer seed 1.
  --target parser (the default) fuzzes the Trade Republic parser. Its seeds are the synthetic CSV
  files under tests/test_data and tests/fuzz/seeds. New inputs go to runtime/fuzz/corpus.
  --target multipart fuzzes the multipart reader of the local server (seeds in
  tests/fuzz/multipart_seeds, corpus runtime/fuzz/corpus-multipart).
  --target answers fuzzes the JSON readers of the answer, delete and upload calls (seeds in
  tests/fuzz/answers_seeds, corpus runtime/fuzz/corpus-answers).
  An input that breaks a check is saved as runtime/fuzz/crash-<hash>.
  --replay runs one saved input again (with the same --target) and runs nothing else. It takes a
  repository-relative path, an absolute host path or the /workspace path that libFuzzer prints.
EOF_USAGE
}

seconds=60
replay_file=""
target="parser"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --target)
            if [[ $# -lt 2 || ! "$2" =~ ^(parser|multipart|answers)$ ]]; then
                echo "--target requires parser, multipart or answers." >&2
                usage >&2
                exit 1
            fi

            target="$2"
            shift 2
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

# The executable, the seed sources (container paths, patterns for find), the longest input, the
# dictionary option and the corpus suffix of the target.
case "$target" in
    parser)
        binary="traderepublic_parser_fuzz"
        seed_dirs="/workspace/tests/test_data /workspace/tests/fuzz/seeds"
        seed_pattern="*.csv"
        max_len=8192
        dict_option="-dict=/workspace/tests/fuzz/traderepublic.dict"
        corpus_suffix=""
        ;;
    multipart)
        binary="multipart_fuzz"
        seed_dirs="/workspace/tests/fuzz/multipart_seeds"
        seed_pattern="*.bin"
        max_len=4096
        dict_option=""
        corpus_suffix="-multipart"
        ;;
    answers)
        binary="answers_fuzz"
        seed_dirs="/workspace/tests/fuzz/answers_seeds"
        seed_pattern="*.bin"
        max_len=4096
        dict_option=""
        corpus_suffix="-answers"
        ;;
esac

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

echo "==> Building the fuzz target $binary..."
compose run --rm -T -e CC=clang -e CXX=clang++ dev \
    cmake --build /workspace/build/fuzz --parallel --target "$binary"

fuzzer="/workspace/build/fuzz/tests/fuzz/$binary"

# The leak check of AddressSanitizer reports a leak at exit even for an empty program on Alpine
# (musl). Valgrind covers memory leaks.
if [[ -n "$replay_file" ]]; then
    echo "==> Replaying $replay_file..."
    compose run --rm -T -e ASAN_OPTIONS=detect_leaks=0 dev "$fuzzer" "/workspace/$replay_file"
    exit 0
fi

echo "==> Fuzzing $target for $seconds seconds (seed 1)..."
mkdir -p "$repo_root/runtime/fuzz"
marker="$(mktemp)"
trap 'rm -f "$marker"' EXIT

fuzz_status=0
compose run --rm -T -e ASAN_OPTIONS=detect_leaks=0 dev sh -c '
    set -eu
    corpus="/workspace/runtime/fuzz/corpus$6"
    seeds="/workspace/runtime/fuzz/seeds$6"
    rm -rf "$seeds"
    mkdir -p "$corpus" "$seeds"
    # Sorted, so every machine numbers the seeds the same way and seed 1 gives the same run.
    n=0
    find $4 -name "$5" | LC_ALL=C sort |
        while IFS= read -r seed; do
            n=$((n + 1))
            cp "$seed" "$seeds/seed-$n"
        done
    "$1" \
        -seed=1 \
        -max_total_time="$2" \
        -max_len="$3" \
        -timeout=10 \
        $7 \
        -artifact_prefix=/workspace/runtime/fuzz/ \
        "$corpus" \
        "$seeds"
' sh "$fuzzer" "$seconds" "$max_len" "$seed_dirs" "$seed_pattern" "$corpus_suffix" "$dict_option" || fuzz_status=$?

if [[ "$fuzz_status" -ne 0 ]]; then
    echo "==> The fuzzer stopped with exit code $fuzz_status." >&2

    # Only the files this run wrote: older findings stay in the folder.
    find "$repo_root/runtime/fuzz" -maxdepth 1 -type f -newer "$marker" \
        \( -name 'crash-*' -o -name 'leak-*' -o -name 'oom-*' -o -name 'slow-unit-*' \
        -o -name 'timeout-*' \) | LC_ALL=C sort |
        while IFS= read -r finding; do
            echo "Replay with: scripts/fuzz.sh --target $target --replay runtime/fuzz/$(basename "$finding")" >&2
        done

    exit "$fuzz_status"
fi

echo "==> No finding in $seconds seconds."
