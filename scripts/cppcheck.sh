#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: cppcheck.sh

  Runs Cppcheck against all active project-owned C++ source and header files
  inside the development container.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

if [[ $# -eq 1 ]]; then
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
fi

ensure_dev_image
ensure_build_tree_writable

if ! compose run --rm -T dev cppcheck --version >/dev/null 2>&1; then
    echo "==> Cppcheck is missing from the development image; rebuilding it..."
    compose build dev
fi

analysis_build_dir="/workspace/build/cppcheck"

echo "==> Generating the CMake compile database..."
compose run --rm -T \
    -e CC=clang \
    -e CXX=clang++ \
    dev cmake \
    -S /workspace \
    -B "$analysis_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_DEBUG_TOOLS=ON

# Ignore findings owned by fetched dependencies. Cppcheck 2.12 also needs lightweight models for
# GoogleTest registration macros and Clang's __SIZEOF_INT128__ built-in in this Linux container.
echo "==> Running Cppcheck..."
compose run --rm -T dev cppcheck \
    --project="$analysis_build_dir/compile_commands.json" \
    "--file-filter=/workspace/src/*" \
    "--file-filter=/workspace/tests/*" \
    "--file-filter=/workspace/tools/*" \
    --check-level=exhaustive \
    --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    --inline-suppr \
    --quiet \
    "--suppress=*:build/cppcheck/_deps/*" \
    --suppress=missingIncludeSystem \
    --template=gcc \
    --relative-paths=/workspace \
    "-DTEST(test_suite_name,test_name)=void cppcheck_test()" \
    "-DTEST_P(test_suite_name,test_name)=void cppcheck_parameterized_test()" \
    "-DINSTANTIATE_TEST_SUITE_P(...)= " \
    -D__SIZEOF_INT128__=16 \
    -j 2

echo "==> Checking standalone project headers..."
compose run --rm -T dev sh -c '
    find /workspace/include -type f \( -name "*.hpp" -o -name "*.h" \) -print0 |
        xargs -0 -r cppcheck \
            --check-level=exhaustive \
            --enable=warning,style,performance,portability \
            --error-exitcode=1 \
            --inline-suppr \
            --quiet \
            --language=c++ \
            --std=c++20 \
            --suppress=missingIncludeSystem \
            --suppress=unusedStructMember \
            --template=gcc \
            --relative-paths=/workspace \
            -D__SIZEOF_INT128__=16 \
            -I /workspace/include
'
