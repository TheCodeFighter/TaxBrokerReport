#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: dump_tr_diagnostics.sh [--output runtime/path.txt] [csv-path ...]

    Parses and merges Trade Republic CSVs in the supplied request order, then
    writes only their structured parser and merger diagnostics as readable text.
    Paths are relative to the repository. Output stays below ignored runtime/.

    Default input:  tmp/TransactionExport.csv
    Default output: runtime/diagnostics/tr_merge_diagnostics.txt
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

output_path="runtime/diagnostics/tr_merge_diagnostics.txt"

if [[ "${1:-}" == "--output" ]]; then
    if [[ $# -lt 2 ]]; then
        echo "--output requires a path." >&2
        exit 1
    fi

    output_path="$2"
    shift 2
fi

if [[ $# -eq 0 ]]; then
    set -- tmp/TransactionExport.csv
fi

# Resolve the destination before writing so ../ and symlinks cannot escape runtime/.
output_absolute="$(realpath -m -- "$repo_root/$output_path")"
runtime_absolute="$repo_root/runtime"

if [[ "$output_path" != runtime/* || "$output_absolute" != "$runtime_absolute/"* ]]; then
    echo "Output must remain below runtime/." >&2
    exit 1
fi

csv_paths=()

for csv_path in "$@"; do
    if [[ "$csv_path" == /* || "$csv_path" == -* || ! -f "$repo_root/$csv_path" ]]; then
        echo "CSV input must be an existing repository-relative path: $csv_path" >&2
        exit 1
    fi

    csv_paths+=("/workspace/$csv_path")
done

for csv_path in "$@"; do
    if [[ "$output_absolute" == "$(realpath -- "$repo_root/$csv_path")" ]]; then
        echo "Output must not overwrite an input CSV." >&2
        exit 1
    fi
done

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"

echo "==> Configuring the isolated Trade Republic diagnostics tool..."
compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Trade Republic diagnostics tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_tr_dump --parallel

echo "==> Writing diagnostic text to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_tr_dump" \
    --diagnostics \
    "/workspace/$output_path" \
    "${csv_paths[@]}"
