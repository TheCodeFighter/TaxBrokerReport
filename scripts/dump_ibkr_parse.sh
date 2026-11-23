#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF_USAGE'
Usage: dump_ibkr_parse.sh [--output runtime/path.txt] xml-path ...

    Parses Interactive Brokers Flex Query XML files with the app's parser and
    writes only counts: the events and rows per kind, and the diagnostics per
    severity and message key. No value, name, ID or path is written.
    Paths are relative to the repository. Output stays below ignored runtime/.
    Options and XML paths can come in any order.

    Example: scripts/dump_ibkr_parse.sh tmp/IBKR/export_1.xml

    Default output: runtime/debug/ibkr_parse_counts.txt
EOF_USAGE
}

output_path="runtime/debug/ibkr_parse_counts.txt"
xml_inputs=()

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
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            xml_inputs+=("$1")
            shift
            ;;
    esac
done

if [[ ${#xml_inputs[@]} -eq 0 ]]; then
    echo "At least one XML file is needed." >&2
    usage >&2
    exit 1
fi

# Resolve the destination before writing so ../ and symlinks cannot escape runtime/.
output_absolute="$(realpath -m -- "$repo_root/$output_path")"
runtime_absolute="$repo_root/runtime"

if [[ "$output_path" != runtime/* || "$output_absolute" != "$runtime_absolute/"* ]]; then
    echo "Output must remain below runtime/." >&2
    exit 1
fi

xml_paths=()

for xml_path in "${xml_inputs[@]}"; do
    if [[ "$xml_path" == /* || "$xml_path" == -* || ! -f "$repo_root/$xml_path" ]]; then
        echo "XML input must be an existing repository-relative path: $xml_path" >&2
        exit 1
    fi

    if [[ "$output_absolute" == "$(realpath -- "$repo_root/$xml_path")" ]]; then
        echo "Output must not overwrite an input file." >&2
        exit 1
    fi

    xml_paths+=("/workspace/$xml_path")
done

mkdir -p "$(dirname -- "$output_absolute")"

ensure_build_tree_writable
ensure_dev_image

debug_build_dir="/workspace/build/debug-tools"
reset_build_dir debug-tools

echo "==> Configuring the isolated Interactive Brokers parser dump tool..."
retry_configure compose run --rm \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$debug_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

echo "==> Building the Interactive Brokers parser dump tool..."
compose run --rm dev cmake --build "$debug_build_dir" --target taxbroker_ibkr_dump --parallel

echo "==> Writing counts to $output_path..."
compose run --rm dev \
    "$debug_build_dir/tools/taxbroker_ibkr_dump" \
    "/workspace/$output_path" \
    "${xml_paths[@]}"
