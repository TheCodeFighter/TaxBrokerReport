#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: dump_tr_merge.sh [csv-path] [merged-output-path] [split-directory]

    Splits one local Trade Republic export into three CSV files, parses them as
    separate sources, and writes the merged data for direct inspection. Generated
    files can contain private financial data and remain below ignored directories.

    csv-path          Path relative to the repository
                      (default: tmp/TransactionExport.csv)
    merged-output     Human-readable merged-data output
                      (default: runtime/debug/tr_merged_debug.txt)
    split-directory   Directory for the three generated CSV inputs
                      (default: runtime/debug/tr_merge_inputs)
EOF
}

if [[ $# -gt 3 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

csv_path="${1:-tmp/TransactionExport.csv}"
output_path="${2:-runtime/debug/tr_merged_debug.txt}"
split_dir="${3:-runtime/debug/tr_merge_inputs}"

if [[ "$csv_path" == /* || "$output_path" == /* || "$split_dir" == /* ]]; then
    echo "Paths must be relative to the repository." >&2
    exit 1
fi

if [[ "$output_path" != runtime/* || "$split_dir" != runtime/* ]]; then
    echo "Generated output and split files must remain below runtime/." >&2
    exit 1
fi

if [[ ! -f "$repo_root/$csv_path" ]]; then
    echo "CSV file does not exist: $repo_root/$csv_path" >&2
    exit 1
fi

data_rows="$(awk 'END { print NR - 1 }' "$repo_root/$csv_path")"
if (( data_rows < 3 )); then
    echo "CSV must contain at least three data rows." >&2
    exit 1
fi

mkdir -p "$repo_root/$split_dir"
split_prefix="$repo_root/$split_dir/tr_merge_part_"
awk -v data_rows="$data_rows" -v prefix="$split_prefix" '
    NR == 1 { header = $0; next }
    {
        part = int((NR - 2) * 3 / data_rows) + 1
        if (part > 3) part = 3
        path = prefix part ".csv"
        if (!(path in initialized)) {
            print header > path
            initialized[path] = 1
        }
        print >> path
    }
' "$repo_root/$csv_path"

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"

echo "==> Created three merge inputs in $split_dir"
echo "==> Configuring the isolated Trade Republic merge dump tool..."
compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Trade Republic merge dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_tr_dump --parallel

echo "==> Writing merged C++ data to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_tr_dump" \
    --merge \
    "/workspace/$output_path" \
    "/workspace/$split_dir/tr_merge_part_1.csv" \
    "/workspace/$split_dir/tr_merge_part_2.csv" \
    "/workspace/$split_dir/tr_merge_part_3.csv"
