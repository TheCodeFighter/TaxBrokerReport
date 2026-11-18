#!/usr/bin/env bash
# Checks the built frontend (build/frontend/dist) against the rules of the server: exactly four
# files, each within its size budget, no foreign host, no inline script or style. It runs inside
# the dev container (scripts/frontend.sh check); `--self-test` also proves that each check fails
# on a damaged copy.
set -euo pipefail

# The four files the server embeds, with their size budget in bytes (not compressed).
declare -A budget=(
    ["index.html"]=4096
    ["assets/app.js"]=131072
    ["assets/app.css"]=15360
    ["assets/logo.webp"]=12288
)

# The only URLs allowed in the files: XML namespace names, which are not addresses to fetch, and
# the address of the project's repository, which is only a link on the About page that the user
# opens (the page never fetches it).
allowed_urls=(
    "https://github.com/TheCodeFighter/TaxBrokerReport"
    "http://www.w3.org/2000/svg"
    "http://www.w3.org/1999/xlink"
    "http://www.w3.org/1998/Math/MathML"
    "http://www.w3.org/XML/1998/namespace"
    "http://www.w3.org/1999/xhtml"
)

usage() {
    echo "Usage: frontend_check.sh [--self-test] <dist folder>" >&2
}

# Prints each problem and returns 1 when there is one.
check_dist() {
    local dist="$1"
    local failed=0
    local file
    local size
    local found
    local expected_list
    local url

    expected_list="$(printf '%s\n' "${!budget[@]}" | sort)"
    found="$(cd "$dist" && find . -type f | sed 's|^\./||' | sort)"

    if [[ "$found" != "$expected_list" ]]; then
        echo "The build must hold exactly these files:" >&2
        echo "$expected_list" | sed 's/^/  /' >&2
        echo "It holds:" >&2
        echo "$found" | sed 's/^/  /' >&2
        failed=1
    fi

    for file in "${!budget[@]}"; do
        if [[ ! -f "$dist/$file" ]]; then
            continue
        fi

        size="$(stat -c %s "$dist/$file")"

        if [[ "$size" -gt "${budget[$file]}" ]]; then
            echo "$file is $size bytes; the budget is ${budget[$file]} bytes" >&2
            failed=1
        fi

        while IFS= read -r url; do
            if ! printf '%s\n' "${allowed_urls[@]}" | grep -qxF -- "$url"; then
                echo "$file has a foreign address: $url" >&2
                failed=1
            fi
        done < <(grep -aoE "https?://[A-Za-z0-9._~:/?#@!\$&*+,;=%-]*" "$dist/$file" || true)
    done

    if [[ -f "$dist/index.html" ]]; then
        # Every <script> needs a src, and there is no <style> or style="" (the content policy).
        if grep -oE '<script[^>]*>' "$dist/index.html" | grep -qv 'src='; then
            echo "index.html has a <script> without src" >&2
            failed=1
        fi

        if grep -qiE '<style|[[:space:]]style=' "$dist/index.html"; then
            echo "index.html has an inline style" >&2
            failed=1
        fi
    fi

    return "$failed"
}

# Runs the check on a damaged copy of dist and expects it to fail.
expect_failure() {
    local description="$1"
    local copy="$2"

    if check_dist "$copy" 2>/dev/null; then
        echo "Self-test failed: the check passed a build with $description" >&2
        return 1
    fi

    echo "==> Self-test ok: the check fails a build with $description"
}

self_test() {
    local dist="$1"
    local work
    local failed=0

    work="$(mktemp -d)"
    trap 'rm -rf -- "$work"' RETURN

    cp -r "$dist" "$work/extra_file" && echo "x" >"$work/extra_file/assets/x.js"
    expect_failure "an extra file" "$work/extra_file" || failed=1

    cp -r "$dist" "$work/foreign_host" &&
        echo 'fetch("https://cdn.example.com/x.js")' >>"$work/foreign_host/assets/app.js"
    expect_failure "a foreign address" "$work/foreign_host" || failed=1

    cp -r "$dist" "$work/inline_script" &&
        echo '<script>alert(1)</script>' >>"$work/inline_script/index.html"
    expect_failure "an inline script" "$work/inline_script" || failed=1

    cp -r "$dist" "$work/inline_style" &&
        echo '<style>body{}</style>' >>"$work/inline_style/index.html"
    expect_failure "an inline style" "$work/inline_style" || failed=1

    cp -r "$dist" "$work/big_script" &&
        head -c 131073 /dev/zero | tr '\0' 'a' >>"$work/big_script/assets/app.js"
    expect_failure "an app.js over the budget" "$work/big_script" || failed=1

    return "$failed"
}

run_self_test=false

if [[ "${1:-}" == "--self-test" ]]; then
    run_self_test=true
    shift
fi

if [[ $# -ne 1 || ! -d "$1" ]]; then
    usage
    exit 1
fi

check_dist "$1"
echo "==> The frontend build passes the checks"

if [[ "$run_self_test" == true ]]; then
    self_test "$1"
fi
