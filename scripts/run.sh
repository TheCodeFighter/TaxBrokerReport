#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

"$script_dir/build.sh" dev

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
	dev ./build/dev/src/taxbroker_server "$@"
