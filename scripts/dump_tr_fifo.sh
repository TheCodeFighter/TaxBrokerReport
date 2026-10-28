#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: dump_tr_fifo.sh [--year YYYY] [--output runtime/path.txt] [csv-path ...]

    Parses and merges Trade Republic CSVs in the supplied request order, prepares
    the selected year and runs FIFO matching. Writes each instrument's status, the
    in-year matches with basis, sale value and gain, and the open lots at
    31 December, then the 30-day loss rule result of each sale at a loss, the
    Doh-KDVP filing rows, the blocked ISINs with their reasons and the units left
    out because they are past the holding period.
    Paths are relative to the repository. The output contains private
    financial data and stays below ignored runtime/.
    Options and CSV paths can come in any order.

    Default year:   the previous calendar year
    Default input:  tmp/TransactionExport.csv
    Default output: runtime/debug/tr_fifo_<year>.txt
EOF
}

year="$(( $(date +%Y) - 1 ))"
output_path=""
csv_inputs=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --year)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]{4}$ ]]; then
                echo "--year requires a four-digit year." >&2
                exit 1
            fi

            year="$2"
            shift 2
            ;;
        --output)
            if [[ $# -lt 2 ]]; then
                echo "--output requires a path." >&2
                exit 1
            fi

            output_path="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            csv_inputs+=("$1")
            shift
            ;;
    esac
done

output_path="${output_path:-runtime/debug/tr_fifo_$year.txt}"

if [[ ${#csv_inputs[@]} -eq 0 ]]; then
    csv_inputs=(tmp/TransactionExport.csv)
fi

# Resolve the destination before writing so ../ and symlinks cannot escape runtime/.
output_absolute="$(realpath -m -- "$repo_root/$output_path")"
runtime_absolute="$repo_root/runtime"

if [[ "$output_path" != runtime/* || "$output_absolute" != "$runtime_absolute/"* ]]; then
    echo "Output must remain below runtime/." >&2
    exit 1
fi

csv_paths=()

for csv_path in "${csv_inputs[@]}"; do
    if [[ "$csv_path" == /* || "$csv_path" == -* || ! -f "$repo_root/$csv_path" ]]; then
        echo "CSV input must be an existing repository-relative path: $csv_path" >&2
        exit 1
    fi

    if [[ "$output_absolute" == "$(realpath -- "$repo_root/$csv_path")" ]]; then
        echo "Output must not overwrite an input CSV." >&2
        exit 1
    fi

    csv_paths+=("/workspace/$csv_path")
done

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"
reset_build_dir debug-tools

echo "==> Configuring the isolated FIFO dump tool..."
compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the FIFO dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_tr_dump --parallel

echo "==> Writing FIFO matching for $year to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_tr_dump" \
    --fifo \
    "$year" \
    "/workspace/$output_path" \
    "${csv_paths[@]}"
