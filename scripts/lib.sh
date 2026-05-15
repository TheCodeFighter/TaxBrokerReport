#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
compose_file="$repo_root/docker/docker-compose.yml"

compose() {
    docker compose -f "$compose_file" "$@"
}

ensure_dev_image() {
    if ! docker image inspect taxbrokerreport-dev >/dev/null 2>&1; then
        compose build dev
    fi
}