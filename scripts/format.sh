#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

usage() {
	cat <<'EOF'
Usage: format.sh [--check]

  Without arguments, formats the C++ sources in place.
  With --check, verifies formatting without modifying files.
EOF
}

check_only=false

if [[ $# -gt 1 ]]; then
	echo "Too many arguments." >&2
	usage >&2
	exit 1
fi

if [[ $# -eq 1 ]]; then
	case "$1" in
		-h|--help)
			usage
			exit 0
			;;
		--check)
			check_only=true
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 1
			;;
	esac
fi

ensure_dev_image

echo "==> Formatting C++ sources..."
if [[ "$check_only" == true ]]; then
	compose run --rm dev sh -lc 'find /workspace/include /workspace/src /workspace/tests -type f \( -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 -r clang-format --dry-run --Werror'
else
	compose run --rm dev sh -lc 'find /workspace/include /workspace/src /workspace/tests -type f \( -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 -r clang-format -i'
fi
