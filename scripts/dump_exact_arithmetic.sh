#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/lib.sh"

usage() {
    cat <<'HELP'
Usage: dump_exact_arithmetic.sh [csv-path] [--output runtime/path.txt]
                                [--split-row ROW NEW_SHARES OLD_SHARES]...

Shows only splits from a local Trade Republic export (default: tmp/TransactionExport.csv).
Writes runtime/debug/exact_arithmetic.txt by default. Real data stays local.
First run without factors to inspect the exported action dates and CSV rows.
Shows data guesses and an imaginary 1 new / 2 old comparison as separate previews.
Supply a verified split factor for each action row to preview adjusted open lots.
For example: --split-row 17 2 1 means 2 new shares for 1 old share at CSV row 17.
Rows are one-based, including the header. Trades rebuild the position internally, but are not printed.
HELP
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

csv_path="tmp/TransactionExport.csv"
output_path="runtime/debug/exact_arithmetic.txt"
if [[ $# -gt 0 && "$1" != --* ]]; then
    csv_path="$1"
    shift
fi

split_arguments=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            output_path="$2"
            shift 2
            ;;
        --split-row)
            [[ $# -ge 4 ]] || { usage >&2; exit 2; }
            split_arguments+=("$1" "$2" "$3" "$4")
            shift 4
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$csv_path" == /* || "$output_path" == /* ]]; then
    echo "Paths must be relative to the repository." >&2
    exit 2
fi

# Resolve traversal and symlinks before allowing private output below ignored runtime/.
csv_absolute="$(realpath -m -- "$repo_root/$csv_path")"
output_absolute="$(realpath -m -- "$repo_root/$output_path")"
if [[ "$csv_absolute" != "$repo_root/"* || "$output_absolute" != "$repo_root/runtime/"* ]]; then
    echo "Input must stay inside the repository; output must stay below runtime/." >&2
    exit 2
fi
if [[ ! -f "$csv_absolute" || "$csv_absolute" == "$output_absolute" ]]; then
    echo "Input must exist and output must not overwrite it." >&2
    exit 2
fi

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"
compose run --rm -T -e CC=clang -e CXX=clang++ dev cmake \
    -S /workspace -B "$debug_build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_DEBUG_TOOLS=ON
compose run --rm -T dev cmake --build "$debug_build_dir" \
    --target taxbroker_exact_dump --parallel

compose run --rm -T dev "$debug_build_dir/tools/taxbroker_exact_dump" \
    "/workspace/${csv_absolute#"$repo_root/"}" \
    "/workspace/${output_absolute#"$repo_root/"}" "${split_arguments[@]}"
cat -- "$output_absolute"
