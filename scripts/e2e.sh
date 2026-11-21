#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF2'
Usage: e2e.sh [--reuse-build] [NAME_REGEX]

  Runs the browser tests (frontend/e2e): Chromium drives the page against the real static
  taxbroker_server, inside the e2e container (the dev image plus Chromium).

    (no option)    builds the production image and copies the static binaries to build/static
                   (scripts/build.sh prod), installs the frontend packages, then runs the tests
    --reuse-build  uses build/static and frontend/node_modules as they are; nothing is built
    NAME_REGEX     runs only the tests whose name matches (a Vitest -t pattern)

  The e2e image is never built here: scripts/build.sh e2e-image builds it once (it needs the dev
  image). A screenshot of every failed test goes to the ignored runtime/e2e/.
EOF2
}

reuse_build=false
name_regex=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        --reuse-build)
            reuse_build=true
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [[ -n "$name_regex" ]]; then
                echo "Too many arguments." >&2
                usage >&2
                exit 1
            fi
            name_regex="$1"
            ;;
    esac
    shift
done

if ! docker image inspect taxbrokerreport-e2e >/dev/null 2>&1; then
    echo "==> The taxbrokerreport-e2e image is missing. Build it with: scripts/build.sh e2e-image" >&2
    exit 1
fi

ensure_build_tree_writable

if [[ "$reuse_build" == true ]]; then
    echo "==> Reusing build/static and frontend/node_modules as they are; nothing is built."
else
    "$script_dir/build.sh" prod
    "$script_dir/frontend.sh" install
fi

if [[ ! -x "$repo_root/build/static/taxbroker_server" ]]; then
    echo "==> build/static/taxbroker_server is missing. Build it with: scripts/build.sh prod" >&2
    exit 1
fi

if [[ ! -d "$repo_root/frontend/node_modules/playwright-core" ]]; then
    echo "==> The frontend packages are missing. Install them with: scripts/frontend.sh install" >&2
    exit 1
fi

# Screenshots of an earlier run would look like failures of this one.
rm -rf "$repo_root/runtime/e2e"
mkdir -p "$repo_root/runtime/e2e"

test_args=(npx vitest run -c vitest.e2e.config.ts)

if [[ -n "$name_regex" ]]; then
    test_args+=(-t "$name_regex")
fi

echo "==> Running the browser tests..."
# HOME is a writable folder for the npm and Chromium caches: the container user has no home.
compose run --rm -T -e HOME=/tmp -e TBR_E2E_SCREENSHOTS=/workspace/runtime/e2e \
    -w /workspace/frontend e2e "${test_args[@]}"
