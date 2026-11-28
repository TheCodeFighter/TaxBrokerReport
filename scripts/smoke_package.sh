#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

# Ubuntu 24.04 (glibc, no musl, no network) proves the packages run outside Alpine. The image is
# pinned by digest; change it on purpose, with the version tag in this comment.
ubuntu_image="ubuntu@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b"

usage() {
    cat <<'EOF2'
Usage: smoke_package.sh PACKAGE_DIR

  Checks the Linux packages that scripts/package.sh made (PACKAGE_DIR, usually build/package):

    - SHA256SUMS matches the three files
    - the tar.gz: entries, modes, owner, `--version`, the report CLI against the golden file,
      the log folder, and the server (start, link, page and assets, SIGHUP, SIGTERM)
    - the deb in a clean Ubuntu 24.04 container without network: install, `--version`, the report
      CLI against the golden file, the menu entry, removal
    - the AppImage in the same container (extract and run, no FUSE): `--version` and the report
      CLI against the golden file; then the server on this computer as for the tar.gz

  Everything runs with a new HOME and runtime folder outside the checkout and a fake `xdg-open`
  first in PATH, so no browser opens. It needs Docker, curl and tar. The summary names every
  check that failed.
EOF2
}

case "${1:-}" in
    -h | --help)
        usage
        exit 0
        ;;
    "")
        echo "PACKAGE_DIR is missing." >&2
        usage >&2
        exit 1
        ;;
esac

package_dir="$(cd -- "$1" && pwd)"
fixture_dir="$repo_root/tests/test_data/cli"
packaging_dir="$repo_root/packaging/linux"

work="$(mktemp -d)"
trap 'rm -rf "${work:?}"' EXIT

# A developer's own settings would move the log, the launch file or the saved profile out of the
# new folders (and into the real ones): the checks run without them.
unset XDG_STATE_HOME XDG_DATA_HOME TBR_LOG_FILE TBR_LAUNCH_FILE TBR_SERVER_HOST TBR_SERVER_PORT

failures=0

pass() {
    echo "  ok: $1"
}

fail() {
    echo "  FAILED: $1" >&2
    failures=$((failures + 1))
}

# check "description" command...: runs the command and reports it.
check() {
    local description="$1"
    shift

    if "$@" >/dev/null 2>&1; then
        pass "$description"
    else
        fail "$description"
    fi
}

# The fake browser opener: it records the file it was asked to open and does nothing else.
mkdir -p "$work/bin"
cat >"$work/bin/xdg-open" <<'EOF2'
#!/bin/sh
echo "$1" >> "$XDG_OPEN_MARKER"
exit 0
EOF2
chmod 755 "$work/bin/xdg-open"

# The project version of CMakeLists.txt; the programs must print the same.
version="$(sed -n 's/^project(TaxBrokerReport VERSION \([0-9.]*\).*/\1/p' "$repo_root/CMakeLists.txt")"

if [[ -z "$version" ]]; then
    echo "The version was not found in CMakeLists.txt." >&2
    exit 1
fi

tar_file="$package_dir/taxbroker-$version-linux-x86_64.tar.gz"
deb_file="$package_dir/taxbroker_${version}_amd64.deb"
appimage_file="$package_dir/TaxBrokerReport-$version-x86_64.AppImage"

echo "==> The packages of version $version"

for file in "$tar_file" "$deb_file" "$appimage_file" "$package_dir/SHA256SUMS"; do
    if [[ ! -f "$file" ]]; then
        echo "Missing: $(basename "$file")" >&2
        exit 1
    fi
done

# ---- P5: the sums
echo "==> SHA256SUMS"
check "sha256sum -c SHA256SUMS" bash -c "cd '$package_dir' && sha256sum -c SHA256SUMS"
check "SHA256SUMS has three lines" test "$(wc -l <"$package_dir/SHA256SUMS")" -eq 3

# ---- P1, P2: the tar
echo "==> tar.gz"
expected_entries="$(printf '%s\n' \
    "taxbroker-$version-linux-x86_64/" \
    "taxbroker-$version-linux-x86_64/LICENSE" \
    "taxbroker-$version-linux-x86_64/README.txt" \
    "taxbroker-$version-linux-x86_64/THIRD_PARTY_NOTICES.txt" \
    "taxbroker-$version-linux-x86_64/taxbroker" \
    "taxbroker-$version-linux-x86_64/taxbroker_server")"
actual_entries="$(tar -tzf "$tar_file")"

if [[ "$actual_entries" == "$expected_entries" ]]; then
    pass "6 entries"
else
    fail "tar entries: $actual_entries"
fi

# Mode, owner and name of every entry (`tar -tv` prints: mode owner/group size date time name).
modes="$(tar -tvzf "$tar_file" | awk '{print $1, $2, $NF}' | sed "s#taxbroker-$version-linux-x86_64/#./#")"
expected_modes="$(printf '%s\n' \
    "drwxr-xr-x 0/0 ./" \
    "-rw-r--r-- 0/0 ./LICENSE" \
    "-rw-r--r-- 0/0 ./README.txt" \
    "-rw-r--r-- 0/0 ./THIRD_PARTY_NOTICES.txt" \
    "-rwxr-xr-x 0/0 ./taxbroker" \
    "-rwxr-xr-x 0/0 ./taxbroker_server")"

if [[ "$modes" == "$expected_modes" ]]; then
    pass "modes and owner 0/0"
else
    fail "tar modes: $modes"
fi

tar -xzf "$tar_file" -C "$work"
tar_dir="$work/taxbroker-$version-linux-x86_64"

for binary in taxbroker taxbroker_server; do
    if [[ "$("$tar_dir/$binary" --version)" == "TaxBrokerReport $version" ]]; then
        pass "tar $binary --version"
    else
        fail "tar $binary --version"
    fi
done

# ---- P7 (tar), P10: the report CLI on this computer, and the log
run_report() {
    # run_report OUT_DIR COMMAND...: the report of the synthetic fixture with the given program.
    local out_dir="$1"
    shift

    "$@" report --year 2024 --taxpayer "$fixture_dir/taxpayer.json" --out "$out_dir" \
        --trade-republic "$fixture_dir/report_fixture.csv" >/dev/null
}

home_tar="$work/home_tar"
mkdir -p "$home_tar"
export XDG_OPEN_MARKER="$work/opened"

if HOME="$home_tar" run_report "$work/out_tar" "$tar_dir/taxbroker" &&
    cmp -s "$work/out_tar/Doh-KDVP_2024.xml" "$fixture_dir/Doh-KDVP_2024.golden.xml"; then
    pass "tar report equals the golden Doh-KDVP_2024.xml"
else
    fail "tar report differs from the golden file"
fi

log_folder="$home_tar/.local/state/taxbroker"

if [[ -f "$log_folder/taxbroker.log" ]]; then
    pass "the log taxbroker.log exists"
else
    fail "no taxbroker.log in the state folder"
fi

if [[ "$(stat -c %a "$log_folder")" == "700" ]]; then
    pass "the log folder has mode 700"
else
    fail "the log folder mode is $(stat -c %a "$log_folder")"
fi

# ---- P9: the server
# check_server LABEL SIGNAL EXIT_CODES COMMAND...: starts the server with new folders, waits for
# its link, fetches the page and the three assets, sends the signal and checks the exit code
# (one of EXIT_CODES, words separated by spaces) and that the launch file is gone. The command
# runs in its own process group and the signal goes to the whole group, as a closed terminal
# window does: the AppImage runtime is the parent of the server and ends with the signal too.
check_server() {
    local label="$1"
    local signal="$2"
    local exit_codes="$3"
    shift 3

    local home="$work/home_$label"
    local runtime="$work/run_$label"
    local output="$work/server_$label.out"
    mkdir -p "$home" "$runtime"
    chmod 700 "$runtime"
    : >"$XDG_OPEN_MARKER"

    HOME="$home" XDG_RUNTIME_DIR="$runtime" PATH="$work/bin:$PATH" TBR_SERVER_PORT=0 \
        setsid "$@" >"$output" 2>&1 &
    local pid=$!

    local port=""
    local attempt

    for attempt in $(seq 1 100); do
        port="$(sed -n 's#^Open http://127.0.0.1:\([0-9]*\)/.*#\1#p' "$output" | head -n 1)"
        [[ -n "$port" ]] && break
        sleep 0.2
    done

    if [[ -z "$port" ]]; then
        fail "$label server printed no link: $(cat "$output")"
        kill -KILL "$pid" 2>/dev/null || true

        return
    fi

    local launch_file="$runtime/taxbroker/launch.html"
    local path

    check "$label launch file exists" test -f "$launch_file"
    check "$label opened the launch file in the browser (fake xdg-open)" grep -q "launch.html" "$XDG_OPEN_MARKER"

    for path in "" assets/app.js assets/app.css assets/logo.webp; do
        if [[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/$path")" == "200" ]]; then
            pass "$label GET /$path is 200"
        else
            fail "$label GET /$path"
        fi
    done

    kill "-$signal" -- "-$pid"

    local exit_code=0
    wait "$pid" || exit_code=$?

    if [[ " $exit_codes " == *" $exit_code "* ]]; then
        pass "$label exits with $exit_code after SIG$signal"
    else
        fail "$label exit code $exit_code after SIG$signal (accepted: $exit_codes)"
    fi

    # The server of an AppImage ends a moment after its parent: wait for the launch file.
    for attempt in $(seq 1 50); do
        [[ ! -e "$launch_file" ]] && break
        sleep 0.1
    done

    if [[ ! -e "$launch_file" ]]; then
        pass "$label launch file is gone"
    else
        fail "$label launch file is still there"
    fi
}

echo "==> The server from the tar.gz"
check_server tar_hup HUP "0" "$tar_dir/taxbroker_server"
check_server tar_term TERM "0" "$tar_dir/taxbroker_server"

# ---- P3, P4, P7, P8: the deb in a clean Ubuntu container
echo "==> The deb in Ubuntu 24.04 (no network)"
uid="$(id -u)"
gid="$(id -g)"
mkdir -p "$work/out_deb"

if docker run --rm --network none \
    -e HOST_UID="$uid" -e HOST_GID="$gid" -e VERSION="$version" \
    -v "$package_dir:/pkg:ro" -v "$fixture_dir:/fixtures:ro" -v "$packaging_dir:/packaging:ro" \
    -v "$work/out_deb:/out" \
    "$ubuntu_image" bash -c '
        set -euo pipefail
        trap "chown -R $HOST_UID:$HOST_GID /out" EXIT

        [ "$(dpkg-deb -f /pkg/taxbroker_${VERSION}_amd64.deb Package)" = "taxbroker" ]
        [ "$(dpkg-deb -f /pkg/taxbroker_${VERSION}_amd64.deb Version)" = "$VERSION" ]
        [ "$(dpkg-deb -f /pkg/taxbroker_${VERSION}_amd64.deb Architecture)" = "amd64" ]
        dpkg-deb -f /pkg/taxbroker_${VERSION}_amd64.deb Maintainer | grep -q "tax.brokerage.report@gmail.com"

        # The nine files of the package (the folders are not counted), two of them links.
        dpkg-deb -c /pkg/taxbroker_${VERSION}_amd64.deb | awk "{print \$1, \$NF}" | grep -v "^d" | \
            sed "s#^\(.\)[^ ]* #\1 #" | sort > /tmp/files.txt
        test "$(wc -l < /tmp/files.txt)" -eq 9
        test "$(grep -c "^l " /tmp/files.txt)" -eq 2

        dpkg -i /pkg/taxbroker_${VERSION}_amd64.deb > /dev/null
        [ "$(taxbroker --version)" = "TaxBrokerReport $VERSION" ]
        [ "$(taxbroker_server --version)" = "TaxBrokerReport $VERSION" ]
        taxbroker report --year 2024 --taxpayer /fixtures/taxpayer.json --out /out \
            --trade-republic /fixtures/report_fixture.csv > /dev/null
        cmp /out/Doh-KDVP_2024.xml /fixtures/Doh-KDVP_2024.golden.xml

        # The menu entry and the icon are in place and the entry is the one of the repository.
        cmp /usr/share/applications/taxbroker.desktop /packaging/taxbroker.desktop
        test -f /usr/share/icons/hicolor/256x256/apps/taxbroker.png
        test -f /usr/share/doc/taxbroker/copyright
        test -f /usr/lib/taxbroker/THIRD_PARTY_NOTICES.txt

        dpkg -r taxbroker > /dev/null
        test ! -e /usr/lib/taxbroker
        test ! -e /usr/share/applications/taxbroker.desktop
        test ! -e /usr/bin/taxbroker
    '; then
    pass "deb: fields, 9 files with 2 links, install, --version, report equals the golden file, removal"
else
    fail "deb check in the container"
fi

# ---- P7, P8: the AppImage in the same container, then the server here
echo "==> The AppImage in Ubuntu 24.04 (no network)"
mkdir -p "$work/out_appimage"

if docker run --rm --network none \
    -e HOST_UID="$uid" -e HOST_GID="$gid" -e VERSION="$version" \
    -e APPIMAGE_EXTRACT_AND_RUN=1 \
    -v "$package_dir:/pkg:ro" -v "$fixture_dir:/fixtures:ro" -v "$work/out_appimage:/out" \
    "$ubuntu_image" bash -c '
        set -euo pipefail
        trap "chown -R $HOST_UID:$HOST_GID /out" EXIT

        appimage="/pkg/TaxBrokerReport-${VERSION}-x86_64.AppImage"

        [ "$("$appimage" --version)" = "TaxBrokerReport $VERSION" ]
        "$appimage" report --year 2024 --taxpayer /fixtures/taxpayer.json --out /out \
            --trade-republic /fixtures/report_fixture.csv > /dev/null
        cmp /out/Doh-KDVP_2024.xml /fixtures/Doh-KDVP_2024.golden.xml
    '; then
    pass "AppImage: --version and the report equals the golden file"
else
    fail "AppImage check in the container"
fi

echo "==> The server from the AppImage"
# The runtime itself ends with the signal (129 after SIGHUP); the server it started must clean up.
APPIMAGE_EXTRACT_AND_RUN=1 check_server appimage_hup HUP "0 129" "$appimage_file"

echo
if [[ "$failures" -eq 0 ]]; then
    echo "All package checks passed."
else
    echo "$failures package check(s) FAILED." >&2
    exit 1
fi
