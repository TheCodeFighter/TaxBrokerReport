#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: build.sh [dev|image|docker-build|prod]

    dev            Remove build/dev, then compile the backend and the debug tools
                   from scratch with Clang + Ninja (default). Needs the dev image.
    image          Build only the dev image.
    docker-build   Build the dev image, then run the dev build.
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
elif [[ "$mode" == "image" ]]; then
    echo "==> Building Docker dev image..."
    compose build dev
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

    ensure_dev_image
    ensure_build_tree_writable
    reset_build_dir dev

    echo "==> Configuring and building the backend inside the dev container..."
    compose run --rm -e CC=clang -e CXX=clang++ dev sh -lc 'cmake -S /workspace -B /workspace/build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_DEBUG_TOOLS=ON && cmake --build /workspace/build/dev --parallel'
    ensure_build_outputs_executable
fi
