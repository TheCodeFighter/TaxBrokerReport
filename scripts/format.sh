#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

usage() {
	cat <<'EOF'
Usage: format.sh [--check] [--] [FILE...]

  Without arguments, formats the C++ sources in place.
  With --check, verifies formatting without modifying files.
  With FILE... (paths relative to the repository root; a leading ./ or the absolute
  repository path is removed), only those files are used. Files outside include/, src/,
  tests/ and tools/, files that are not C++ and missing files are skipped. If none is
  left, nothing runs; "--" with no file after it also runs nothing (never all files).
  The Git hooks use this with the changed files.
EOF
}

check_only=false
file_mode=false
files=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h|--help)
			usage
			exit 0
			;;
		--check)
			check_only=true
			;;
		--)
			shift
			file_mode=true
			files+=("$@")
			break
			;;
		-*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 1
			;;
		*)
			file_mode=true
			files+=("$1")
			;;
	esac
	shift
done

if [[ "$file_mode" == true ]]; then
	selected=()
	for f in ${files[@]+"${files[@]}"}; do
		f="${f#"$repo_root"/}"
		while [[ "$f" == ./* ]]; do
			f="${f#./}"
		done
		case "$f" in
			include/*|src/*|tests/*|tools/*) ;;
			*) continue ;;
		esac
		case "$f" in
			*.cpp|*.cc|*.cxx|*.hpp|*.h) ;;
			*) continue ;;
		esac
		if [[ -f "$repo_root/$f" ]]; then
			selected+=("/workspace/$f")
		fi
	done

	if [[ ${#selected[@]} -eq 0 ]]; then
		echo "==> No C++ files to format."
		exit 0
	fi

	ensure_dev_image

	echo "==> Formatting ${#selected[@]} C++ file(s)..."
	if [[ "$check_only" == true ]]; then
		compose run --rm dev sh -lc 'printf "%s\0" "$@" | xargs -0 -r -P "$(nproc)" -n 4 clang-format --dry-run --Werror' sh "${selected[@]}"
	else
		compose run --rm dev sh -lc 'printf "%s\0" "$@" | xargs -0 -r -P "$(nproc)" -n 4 clang-format -i' sh "${selected[@]}"
	fi
	exit 0
fi

ensure_dev_image

echo "==> Formatting C++ sources..."
if [[ "$check_only" == true ]]; then
	compose run --rm dev sh -lc 'find /workspace/include /workspace/src /workspace/tests /workspace/tools -type f \( -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 -r -P "$(nproc)" -n 4 clang-format --dry-run --Werror'
else
	compose run --rm dev sh -lc 'find /workspace/include /workspace/src /workspace/tests /workspace/tools -type f \( -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" -o -name "*.hpp" -o -name "*.h" \) -print0 | xargs -0 -r -P "$(nproc)" -n 4 clang-format -i'
fi
