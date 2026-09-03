#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: dump_tr_parse.sh [csv-path] [parsed-output-path] [diagnostics-output-path]

    csv-path                Path relative to the repository
                            (default: tmp/TransactionExport.csv)
    parsed-output-path      Human-readable parsed-data output
                            (default: runtime/debug/tr_parsed_debug.txt)
    diagnostics-output-path Versioned JSON diagnostics output
                            (default: runtime/diagnostics/tr_parse_diagnostics.json)
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
output_path="${2:-runtime/debug/tr_parsed_debug.txt}"
diagnostics_output_path="${3:-runtime/diagnostics/tr_parse_diagnostics.json}"

if [[ "$csv_path" == /* || "$output_path" == /* || "$diagnostics_output_path" == /* ]]; then
    echo "Paths must be relative to the repository." >&2
    exit 1
fi

if [[ ! -f "$repo_root/$csv_path" ]]; then
    echo "CSV file does not exist: $repo_root/$csv_path" >&2
    exit 1
fi

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"

echo "==> Configuring the isolated Trade Republic parse dump tool..."
compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Trade Republic parse dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_tr_dump --parallel

echo "==> Writing parsed C++ data to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_tr_dump" \
    "/workspace/$csv_path" \
    "/workspace/$output_path" \
    "/workspace/$diagnostics_output_path"
