#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

# Docker older than 28.0 lets other computers on the network reach a port published on 127.0.0.1.
docker_version="$(docker version --format '{{.Server.Version}}')"
if [[ "${docker_version%%.*}" -lt 28 ]]; then
	echo "==> Docker Engine 28.0 or later is needed (found $docker_version): older versions expose the 127.0.0.1 port to the local network" >&2
	exit 1
fi

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
