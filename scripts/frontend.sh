#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF2'
Usage: frontend.sh [--rebuild-all] [install|typecheck|test|build|check|all]

  Builds and checks the frontend (frontend/) inside the dev container. The result is the four
  files in build/frontend/dist that the server embeds (TAXBROKER_FRONTEND_DIR).

    install    npm ci --ignore-scripts: installs exactly the versions of package-lock.json and
               never runs an install script of a dependency
    typecheck  tsc --noEmit (strict)
    test       the Vitest tests
    build      Vite build to build/frontend/dist
    check      the size budget and content rules of the four files, with a self-test that each
               rule fails on a damaged copy
    all        install, typecheck, test, build and check (default)

  --rebuild-all  installs the packages again first (npm ci), even when node_modules looks
                 current, for install, typecheck, test and build (build always writes an empty
                 build/frontend/dist). Without it, build installs only when node_modules is
                 missing or package-lock.json is newer.
EOF2
}

rebuild_all=false
mode=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebuild-all)
            rebuild_all=true
            ;;
        *)
            if [[ -n "$mode" ]]; then
                echo "Too many arguments." >&2
                usage >&2
                exit 1
            fi
            mode="$1"
            ;;
    esac
    shift
done

mode="${mode:-all}"

# HOME is a writable folder for the npm cache: the container user has no home of its own.
npm_run() {
    compose run --rm -T -e HOME=/tmp -w /workspace/frontend dev "$@"
}

run_install() {
    echo "==> Installing the frontend packages (npm ci --ignore-scripts)..."
    npm_run npm ci --ignore-scripts
}

run_typecheck() {
    echo "==> Type check..."
    npm_run npm run typecheck
}

run_test() {
    echo "==> Frontend tests..."
    npm_run npm test
}

run_build() {
    echo "==> Building the frontend to build/frontend/dist..."
    ensure_build_tree_writable

    # run.sh builds without an earlier install. npm ci writes node_modules/.package-lock.json, so a
    # package-lock.json that is newer (after a pull) means the packages are out of date.
    local modules="$repo_root/frontend/node_modules"

    if [[ "$rebuild_all" == true || ! -d "$modules" ||
        "$repo_root/frontend/package-lock.json" -nt "$modules/.package-lock.json" ]]; then
        run_install
    fi

    mkdir -p "$repo_root/build/frontend"
    npm_run npm run build
}

run_check() {
    echo "==> Checking the frontend build..."
    compose run --rm -T dev bash /workspace/scripts/frontend_check.sh --self-test \
        /workspace/build/frontend/dist
}

case "$mode" in
    -h | --help)
        usage
        ;;
    install)
        ensure_dev_image
        run_install
        ;;
    typecheck)
        ensure_dev_image
        if [[ "$rebuild_all" == true ]]; then run_install; fi
        run_typecheck
        ;;
    test)
        ensure_dev_image
        if [[ "$rebuild_all" == true ]]; then run_install; fi
        run_test
        ;;
    build)
        ensure_dev_image
        run_build
        ;;
    check)
        ensure_dev_image
        run_check
        ;;
    all)
        ensure_dev_image
        run_install
        # The packages were just installed, so the build does not install them a second time.
        rebuild_all=false
        run_typecheck
        run_test
        run_build
        run_check
        ;;
    *)
        echo "Unknown mode: $mode" >&2
        usage >&2
        exit 1
        ;;
esac
