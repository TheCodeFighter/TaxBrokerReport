#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

usage() {
    cat <<'EOF'
Usage: build.sh [dev|prod]

  dev   Build the dev image and compile the backend with Clang + Ninja.
  prod  Build the production image.
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
    if [[ "$mode" != "dev" ]]; then
        echo "Unknown build mode: $mode" >&2
        usage >&2
        exit 1
    fi

    echo "==> Building Docker dev image..."
    compose build dev

    echo "==> Configuring and building the backend inside the dev container..."
    compose run --rm -e CC=clang -e CXX=clang++ dev sh -lc 'cmake -S /workspace -B /workspace/build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /workspace/build --parallel'
fi
