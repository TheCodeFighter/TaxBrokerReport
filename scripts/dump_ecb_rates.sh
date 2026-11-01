#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF2'
Usage: dump_ecb_rates.sh [--output path] ecb-csv...

    Loads ECB exchange-rate files (the unmodified Data Portal csvdata export of EXR, one
    calendar year per file) with the same code as the application and writes, per file, its name,
    year, SHA-256, number of rates and currencies, first and last date, then every error.
    Exit code 1 when a file is rejected.

    ecb-csv   One or more paths relative to the repository (for example runtime/ecb/exr_2025.csv)
    --output  Summary file (default: runtime/debug/ecb_rates_summary.txt)

    Download one year:
    https://data-api.ecb.europa.eu/service/data/EXR/D..EUR.SP00.A?startPeriod=2025-01-01&endPeriod=2025-12-31&format=csvdata
EOF2
}

output_path="runtime/debug/ecb_rates_summary.txt"
inputs=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
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
            inputs+=("$1")
            shift
            ;;
    esac
done

if [[ ${#inputs[@]} -eq 0 ]]; then
    echo "At least one rate file is required." >&2
    usage >&2
    exit 1
fi

for path in "${inputs[@]}" "$output_path"; do
    if [[ "$path" == /* ]]; then
        echo "Paths must be relative to the repository." >&2
        exit 1
    fi
done

for path in "${inputs[@]}"; do
    if [[ ! -f "$repo_root/$path" ]]; then
        echo "Rate file does not exist: $path" >&2
        exit 1
    fi
done

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"
reset_build_dir debug-tools

echo "==> Configuring the isolated ECB rate dump tool..."
retry_configure compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the ECB rate dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_ecb_dump --parallel

mkdir -p "$repo_root/$(dirname "$output_path")"

container_inputs=()
for path in "${inputs[@]}"; do
    container_inputs+=("/workspace/$path")
done

echo "==> Writing the summary to $output_path..."
status=0
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_ecb_dump" \
    "/workspace/$output_path" \
    "${container_inputs[@]}" || status=$?

cat "$repo_root/$output_path"
exit "$status"
