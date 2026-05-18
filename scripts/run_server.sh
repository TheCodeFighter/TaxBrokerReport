#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

if [[ ! -f "$repo_root/build/build.ninja" ]]; then
	echo "==> Build tree not found; building the dev image and backend first..."
	"$script_dir/build.sh" dev
else
	ensure_dev_image
	echo "==> Refreshing the backend build..."
	compose run --rm dev sh -lc 'cmake --build /workspace/build --parallel'
fi

echo "==> Running taxbroker_server in development mode on port 8080..."
compose run --rm --service-ports \
	-e TBR_LOG_LEVEL="${TBR_LOG_LEVEL:-}" \
	-e TBR_LOG_FILE="${TBR_LOG_FILE:-}" \
	-e TBR_LOG_FILE_MODE="${TBR_LOG_FILE_MODE:-}" \
	dev ./build/src/taxbroker_server "$@"
