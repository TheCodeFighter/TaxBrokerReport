#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

bash "$script_dir/restore_exec_bits.sh"

ensure_build_tree_writable

if [[ ! -f "$repo_root/build/build.ninja" ]]; then
	echo "==> Build tree not found; building the dev image and backend first..."
	"$script_dir/build.sh" dev
else
	ensure_dev_image
	echo "==> Refreshing the backend build..."
	compose run --rm dev sh -lc 'cmake --build /workspace/build --parallel'
fi

ensure_build_outputs_executable

echo "==> Running taxbroker_server in development mode on port 8080..."
TBR_LOG_FILE="${TBR_LOG_FILE:-/var/log/taxbroker/taxbroker.log}"
export TBR_LOG_FILE
ensure_log_path_writable
if ! container_log_file="$(resolve_container_log_file_path)"; then
	cat <<EOF >&2
==> Unsupported TBR_LOG_FILE path: $TBR_LOG_FILE
==> Use a relative path, /workspace/..., or /var/log/taxbroker/...
EOF
	exit 1
fi
compose run --rm --service-ports \
	-e TBR_LOG_LEVEL="${TBR_LOG_LEVEL:-}" \
	-e TBR_LOG_FILE="${container_log_file}" \
	-e TBR_LOG_FILE_MODE="${TBR_LOG_FILE_MODE:-}" \
	dev ./build/src/taxbroker_server "$@"
