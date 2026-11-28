#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
    cat <<'EOF2'
Usage: package.sh

  Makes the Linux packages in build/package (the folder is emptied first):

    taxbroker-<version>-linux-x86_64.tar.gz
    taxbroker_<version>_amd64.deb
    TaxBrokerReport-<version>-x86_64.AppImage
    SHA256SUMS

  The static binaries come from the production image (the builder stage of docker/Dockerfile),
  so a source change builds the whole application again; run it after scripts/build.sh prod to
  reuse that build. The files are reproducible: the time of every file is the time of the last
  commit (SOURCE_DATE_EPOCH), so two runs on the same commit give the same SHA256SUMS.

  Check the result with scripts/smoke_package.sh build/package.
EOF2
}

case "${1:-}" in
    -h | --help)
        usage
        exit 0
        ;;
    "") ;;
    *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 1
        ;;
esac

ensure_build_tree_writable

# The time of the last commit is the time of every file in the packages.
SOURCE_DATE_EPOCH="$(git -C "$repo_root" log -1 --format=%ct)"
export SOURCE_DATE_EPOCH

echo "==> Building the package image (the static binaries and the packaging tools)..."
compose build package

# The folder is made here, so Docker does not create it as root; it is emptied for a fresh run.
package_dir="$repo_root/build/package"
rm -rf -- "${package_dir:?}"
mkdir -p "$package_dir"

echo "==> Making the packages in build/package..."
compose run --rm -T package

echo "==> Done:"
ls -l "$package_dir"
