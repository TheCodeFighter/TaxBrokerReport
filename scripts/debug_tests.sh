#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: debug_tests.sh build unit|integration

  Builds the selected test executable inside Docker with optimizations disabled
  and full debug information, using the isolated build/test-debug directory.

  The gdb mode is the internal VS Code transport; it starts GDB inside Docker.
EOF
}

mode="${1:-}"

case "$mode" in
    -h | --help)
        usage
        exit 0
        ;;
    build)
        if [[ $# -ne 2 || ( "$2" != "unit" && "$2" != "integration" ) ]]; then
            usage >&2
            exit 1
        fi

        ensure_build_tree_writable
        ensure_dev_image

        echo "==> Building $2 tests for debugging inside Docker..."
        compose run --rm --no-deps -T -e CC=clang -e CXX=clang++ dev \
            cmake -S /workspace -B /workspace/build/test-debug -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            '-DCMAKE_CXX_FLAGS_DEBUG=-g3 -O0 -fno-omit-frame-pointer -fstandalone-debug' \
            -DBUILD_LEGACY_GUI=OFF \
            -DBUILD_DEBUG_TOOLS=OFF

        compose run --rm --no-deps -T dev \
            cmake --build /workspace/build/test-debug \
            --target "taxbroker_${2}_tests" --parallel
        ;;
    gdb)
        shift

        if [[ "${1:-}" != "/usr/bin/gdb" ]]; then
            echo "The VS Code transport requires /usr/bin/gdb." >&2
            exit 1
        fi

        # Keep stdout free of script messages: it carries GDB's MI protocol.
        compose run --rm --no-deps -T dev "$@"
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
