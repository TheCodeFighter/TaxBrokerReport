#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

synthetic_csv="tests/test_data/csv/kdvp_dump_fixture.csv"

usage() {
    cat <<EOF
Usage: dump_tr_kdvp.sh [--synthetic] [--year YYYY] [--tax-number NNNNNNNN] [--self-report]
                       [--output runtime/path.xml] [csv-path ...]

    Parses and merges Trade Republic CSVs in the supplied request order, prepares
    the selected year, runs FIFO matching and writes the Doh-KDVP XML of the filing
    rows for a resident taxpayer. Blocked ISINs are left out, so with a blocked ISIN
    the file is not a complete return; the tool prints a warning and
    dump_tr_fifo.sh shows the reasons. Paths are relative to the repository. The
    output can contain private financial data and stays below ignored runtime/.

    --synthetic     use the synthetic CSV $synthetic_csv
                    (trades in 2025) instead of the real export
    --tax-number    the taxpayer's tax number; use your own for an eDavki draft
                    import (default: 12345678, a made-up number)
    --self-report   write the self-report workflow code I instead of O

    Default year:   the previous calendar year
    Default input:  tmp/TransactionExport.csv
    Default output: runtime/debug/tr_kdvp_<year>.xml, or
                    runtime/debug/tr_kdvp_synthetic_<year>.xml with --synthetic
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

year="$(( $(date +%Y) - 1 ))"
output_path=""
tax_number="12345678"
filing_type="original"
synthetic=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --synthetic)
            synthetic=true
            shift
            ;;
        --year)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]{4}$ ]]; then
                echo "--year requires a four-digit year." >&2
                exit 1
            fi

            year="$2"
            shift 2
            ;;
        --tax-number)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]{8}$ ]]; then
                echo "--tax-number requires eight digits." >&2
                exit 1
            fi

            tax_number="$2"
            shift 2
            ;;
        --self-report)
            filing_type="self-report"
            shift
            ;;
        --output)
            if [[ $# -lt 2 ]]; then
                echo "--output requires a path." >&2
                exit 1
            fi

            output_path="$2"
            shift 2
            ;;
        *)
            break
            ;;
    esac
done

if [[ "$synthetic" == true ]]; then
    if [[ $# -gt 0 ]]; then
        echo "--synthetic takes no CSV paths." >&2
        exit 1
    fi

    set -- "$synthetic_csv"
    output_path="${output_path:-runtime/debug/tr_kdvp_synthetic_$year.xml}"
fi

output_path="${output_path:-runtime/debug/tr_kdvp_$year.xml}"

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

echo "==> Configuring the isolated Doh-KDVP dump tool..."
compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Doh-KDVP dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_tr_dump --parallel

echo "==> Writing Doh-KDVP XML for $year to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_tr_dump" \
    --kdvp \
    "$year" \
    "/workspace/$output_path" \
    "$tax_number" \
    "$filing_type" \
    "${csv_paths[@]}"
