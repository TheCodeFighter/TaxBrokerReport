#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
	cat <<'EOF'
Usage: run.sh [--reuse-build-backend] [--reuse-build-frontend] [--reuse-build] [--no-browser]

  With no build flag, builds build/dev from scratch and the frontend (scripts/frontend.sh build),
  embeds the frontend in taxbroker_server, runs it in the development container on
  127.0.0.1:8080 and opens the launch file in the default browser of the host.
  --reuse-build-backend  keeps the existing build/dev (no build from scratch). The frontend is
                         built again and the server is rebuilt with it (only what changed is
                         compiled). Use it after a change in frontend/src.
  --reuse-build-frontend keeps the existing build/frontend/dist (no frontend build). build/dev is
                         built from scratch and embeds that frontend. Use it after a C++ change.
  --reuse-build          both of the above: nothing is built, the existing server runs as it is.
                         It needs a server that run.sh built before.
  --no-browser           does not open the launch file, so the one-time link printed by the
                         server stays unused and can be redeemed by hand (see "Manual API test"
                         in docs/debugging.md).
EOF
}

reuse_backend=false
reuse_frontend=false
open_browser=true

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h | --help)
			usage
			exit 0
			;;
		--reuse-build-backend)
			reuse_backend=true
			shift
			;;
		--reuse-build-frontend)
			reuse_frontend=true
			shift
			;;
		--reuse-build)
			reuse_backend=true
			reuse_frontend=true
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

frontend_index="$repo_root/build/frontend/dist/index.html"
server_binary="$dev_build_dir/src/taxbroker_server"

# Backend: build/dev from scratch, or the existing one.
if [[ "$reuse_backend" == true ]]; then
	ensure_dev_image
	ensure_build_tree_writable
	ensure_dev_build_exists
	echo "==> Reusing build/dev (no build from scratch)."
else
	"$script_dir/build.sh" dev
fi

# Frontend: build it, or use the existing build/frontend/dist.
if [[ "$reuse_frontend" == true ]]; then
	if [[ ! -f "$frontend_index" ]]; then
		echo "==> build/frontend/dist has no frontend. Build it with: scripts/frontend.sh build" >&2
		exit 1
	fi
	echo "==> Reusing build/frontend/dist (the frontend is not built)."
else
	"$script_dir/frontend.sh" build
fi

# The server embeds the four frontend files. A configure with the same folder again rebuilds
# only what changed: the frontend files, the generated source and the server. With both parts
# reused nothing changed, so the existing server runs as it is.
if [[ "$reuse_backend" == true && "$reuse_frontend" == true ]]; then
	if [[ ! -x "$server_binary" ]] || ! grep -q '^TAXBROKER_FRONTEND_DIR:PATH=.\+' "$dev_build_dir/CMakeCache.txt"; then
		echo "==> build/dev has no taxbroker_server with the embedded frontend. Build it with: scripts/run.sh" >&2
		exit 1
	fi
else
	echo "==> Embedding the frontend in taxbroker_server..."
	retry_configure compose run --rm -T -e CC=clang -e CXX=clang++ dev sh -lc \
		'cmake -S /workspace -B /workspace/build/dev -DTAXBROKER_FRONTEND_DIR=/workspace/build/frontend/dist'
	compose run --rm -T -e CC=clang -e CXX=clang++ dev sh -lc \
		'cmake --build /workspace/build/dev --target taxbroker_server --parallel'
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
