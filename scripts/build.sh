#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

bash "$script_dir/restore_exec_bits.sh"

usage() {
    cat <<'EOF'
Usage: build.sh [dev|prod|docker-build]

    dev            Compile the backend with Clang + Ninja (default).
    docker-build   Build the dev image, then compile the backend.
    prod           Build the production image.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

mode="${1:-dev}"

if [[ "$mode" == "-h" || "$mode" == "--help" ]]; then
    usage
elif [[ "$mode" == "prod" ]]; then
    echo "==> Building Docker prod image..."
    compose build prod
else
    if [[ "$mode" != "dev" && "$mode" != "docker-build" ]]; then
        echo "Unknown build mode: $mode" >&2
        usage >&2
        exit 1
    fi

    if [[ "$mode" == "docker-build" ]]; then
        echo "==> Building Docker dev image..."
        compose build dev
    fi

    echo "==> Configuring and building the backend inside the dev container..."
    ensure_build_tree_writable
    compose run --rm -e CC=clang -e CXX=clang++ dev sh -lc 'cmake -S /workspace -B /workspace/build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /workspace/build --parallel'
    ensure_build_outputs_executable
fi
