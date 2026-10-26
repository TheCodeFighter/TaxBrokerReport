#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF'
Usage: cppcheck.sh [--reuse-build]

  Runs Cppcheck against all active project-owned C++ source and header files
  inside the development container. By default, build/cppcheck is removed and
  configured from scratch to create the compile database.
  --reuse-build uses the compile database of the existing build/dev instead.
EOF
}

if [[ $# -gt 1 ]]; then
    echo "Too many arguments." >&2
    usage >&2
    exit 1
fi

reuse_build=false

if [[ $# -eq 1 ]]; then
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        --reuse-build)
            reuse_build=true
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
    echo "==> Cppcheck is missing from the development image. Rebuild it with: scripts/build.sh image" >&2
    exit 1
fi

if [[ "$reuse_build" == true ]]; then
    ensure_dev_build_exists
    analysis_build_name="dev"
    echo "==> Reusing the compile database of build/dev as it is."
else
    analysis_build_name="cppcheck"
    reset_build_dir "$analysis_build_name"

    echo "==> Generating the CMake compile database..."
    retry_configure compose run --rm -T \
        -e CC=clang \
        -e CXX=clang++ \
        dev cmake \
        -S /workspace \
        -B "/workspace/build/$analysis_build_name" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_DEBUG_TOOLS=ON
fi

analysis_build_dir="/workspace/build/$analysis_build_name"

# Ignore findings owned by fetched dependencies. Cppcheck also needs lightweight models for
# GoogleTest registration macros and Clang's __SIZEOF_INT128__ built-in in this Linux container.
# Cppcheck does not predefine __cpp_exceptions; without it csv.hpp stops with #error, and the
# _deps suppression hides that every file including csv.hpp is skipped.
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
    "--suppress=*:build/$analysis_build_name/_deps/*" \
    --suppress=missingIncludeSystem \
    --template=gcc \
    --relative-paths=/workspace \
    "-DTEST(test_suite_name,test_name)=void cppcheck_test()" \
    "-DTEST_P(test_suite_name,test_name)=void cppcheck_parameterized_test()" \
    "-DINSTANTIATE_TEST_SUITE_P(...)= " \
    -D__SIZEOF_INT128__=16 \
    -D__cpp_exceptions=199711L \
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
