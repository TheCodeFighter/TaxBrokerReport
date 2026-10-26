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
    prod           Build the production image and copy its static taxbroker and
                   taxbroker_server binaries to build/static.
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

    # The static binaries are written by the host user, so a plain rm empties the folder.
    echo "==> Copying the static binaries to build/static..."
    ensure_build_tree_writable
    static_dir="$repo_root/build/static"
    rm -rf -- "$static_dir"
    mkdir -p "$static_dir"

    # The trap removes the helper container also when a copy fails.
    container_id="$(docker create taxbrokerreport-prod)"
    trap 'docker rm "$container_id" >/dev/null' EXIT
    docker cp "$container_id:/app/taxbroker" "$static_dir/taxbroker"
    docker cp "$container_id:/app/taxbroker_server" "$static_dir/taxbroker_server"
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

    echo "==> Configuring the backend inside the dev container..."
    retry_configure compose run --rm -e CC=clang -e CXX=clang++ dev sh -lc 'cmake -S /workspace -B /workspace/build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_DEBUG_TOOLS=ON'

    echo "==> Building the backend inside the dev container..."
    compose run --rm -e CC=clang -e CXX=clang++ dev sh -lc 'cmake --build /workspace/build/dev --parallel'
    ensure_build_outputs_executable
fi
