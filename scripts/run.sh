#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
	cat <<'EOF'
Usage: run.sh [--reuse-build] [--no-browser]

  With no arguments, builds build/dev from scratch, then runs taxbroker_server in the
  development container on 127.0.0.1:8080 and opens the launch file in the default browser
  of the host.
  --reuse-build runs the existing build/dev as it is and compiles nothing.
  --no-browser does not open the launch file, so the one-time link printed by the server stays
  unused and can be redeemed by hand (see "Manual API test" in docs/debugging.md).
EOF
}

reuse_build=false
open_browser=true

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h | --help)
			usage
			exit 0
			;;
		--reuse-build)
			reuse_build=true
			shift
			;;
		--no-browser)
			open_browser=false
			shift
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage >&2
			exit 1
			;;
	esac
done

# Docker older than 28.0 lets other computers on the network reach a port published on 127.0.0.1.
docker_version="$(docker version --format '{{.Server.Version}}')"
if [[ "${docker_version%%.*}" -lt 28 ]]; then
	echo "==> Docker Engine 28.0 or later is needed (found $docker_version): older versions expose the 127.0.0.1 port to the local network" >&2
	exit 1
fi

prepare_dev_build "$reuse_build"

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

# The server writes the launch file (with the one-time link) into the shared workspace; the host
# opens it, because a browser cannot be started inside the container. The link is never an argument.
launch_dir="$repo_root/runtime/launch"
launch_file="$launch_dir/launch.html"
mkdir -p "$launch_dir"
rm -f -- "$launch_file"

if [[ "$open_browser" == true ]] && command -v xdg-open >/dev/null 2>&1; then
	(
		for _ in $(seq 60); do
			if [[ -f "$launch_file" ]]; then
				xdg-open "$launch_file" >/dev/null 2>&1
				exit 0
			fi
			sleep 1
		done
	) &
	watcher=$!
	trap 'kill "$watcher" 2>/dev/null || true' EXIT
fi

compose run --rm --service-ports \
	-e TBR_LAUNCH_FILE=/workspace/runtime/launch/launch.html \
	-e TBR_LOG_LEVEL="${TBR_LOG_LEVEL:-}" \
	-e TBR_LOG_FILE="${container_log_file}" \
	-e TBR_LOG_FILE_MODE="${TBR_LOG_FILE_MODE:-}" \
	dev ./build/dev/src/taxbroker_server
