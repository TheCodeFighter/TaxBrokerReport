#!/usr/bin/env bash
# Makes the three Linux packages and SHA256SUMS from the two static binaries. It runs inside the
# package image (docker/Dockerfile, stage `package`; scripts/package.sh runs it) with the inputs
# in /package and writes the four files to /out, which must be an empty folder.
#
# The files are made reproducible: SOURCE_DATE_EPOCH (the time of the last commit) is the time of
# every file, the owner is root, the order is sorted, gzip stores no name or time.
set -euo pipefail

input="/package"
output="/out"
work="$(mktemp -d)"
trap 'rm -rf "${work:?}"' EXIT

: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH must be set (scripts/package.sh sets it)}"
export SOURCE_DATE_EPOCH

version_line="$("$input/taxbroker" --version)"
version="${version_line#TaxBrokerReport }"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "The version is not x.y.z: $version_line" >&2
    exit 1
fi

if [[ "$("$input/taxbroker_server" --version)" != "$version_line" ]]; then
    echo "taxbroker and taxbroker_server have different versions." >&2
    exit 1
fi

tar_name="taxbroker-$version-linux-x86_64.tar.gz"
deb_name="taxbroker_${version}_amd64.deb"
appimage_name="TaxBrokerReport-$version-x86_64.AppImage"

# Sets the time of everything under a folder to the source date (the folder too).
set_times() {
    find "$1" -print0 | xargs -0 touch -h -d "@$SOURCE_DATE_EPOCH"
}

# The README with the version.
sed "s/@VERSION@/$version/g" "$input/packaging/README.txt" >"$work/README.txt"

# The icon: the project image as a 256 x 256 PNG without date chunks.
magick "$input/icon.jpg" -resize 256x256 -background none -gravity center -extent 256x256 \
    -strip -define png:exclude-chunks=date,time "$work/taxbroker.png"

mkdir -p "$output"

# ---- 1. tar.gz: one folder to unpack and run
folder="taxbroker-$version-linux-x86_64"
mkdir -p "$work/tar/$folder"
install -m 0755 "$input/taxbroker" "$input/taxbroker_server" "$work/tar/$folder/"
install -m 0644 "$input/LICENSE" "$input/THIRD_PARTY_NOTICES.txt" "$work/tar/$folder/"
install -m 0644 "$work/README.txt" "$work/tar/$folder/README.txt"
chmod 0755 "$work/tar/$folder"
set_times "$work/tar"
tar -C "$work/tar" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$SOURCE_DATE_EPOCH" --format=gnu -cf - "$folder" | gzip -n -9 >"$output/$tar_name"

# ---- 2. deb: the programs in /usr/lib/taxbroker, links in /usr/bin
deb="$work/deb"
mkdir -p "$deb/DEBIAN" "$deb/usr/lib/taxbroker" "$deb/usr/bin" "$deb/usr/share/applications" \
    "$deb/usr/share/icons/hicolor/256x256/apps" "$deb/usr/share/doc/taxbroker"
install -m 0755 "$input/taxbroker" "$input/taxbroker_server" "$deb/usr/lib/taxbroker/"
install -m 0644 "$input/LICENSE" "$input/THIRD_PARTY_NOTICES.txt" "$deb/usr/lib/taxbroker/"
ln -s ../lib/taxbroker/taxbroker "$deb/usr/bin/taxbroker"
ln -s ../lib/taxbroker/taxbroker_server "$deb/usr/bin/taxbroker_server"
install -m 0644 "$input/packaging/taxbroker.desktop" "$deb/usr/share/applications/taxbroker.desktop"
install -m 0644 "$work/taxbroker.png" "$deb/usr/share/icons/hicolor/256x256/apps/taxbroker.png"

# The copyright file: the licence text is indented by one space, an empty line is " .".
awk -v licence="$input/LICENSE" '
    $0 == "@MIT_TEXT@" {
        while ((getline line < licence) > 0) { print (line == "" ? " ." : " " line) }
        next
    }
    { print }
' "$input/packaging/copyright.in" >"$deb/usr/share/doc/taxbroker/copyright"
chmod 0644 "$deb/usr/share/doc/taxbroker/copyright"

# The size in KiB of the files, without the control folder. The apparent size does not depend
# on the file system, so the control file (and the .deb) is the same on every computer.
installed_size="$(du -sk --apparent-size --exclude=DEBIAN "$deb" | cut -f1)"
sed -e "s/@VERSION@/$version/" -e "s/@INSTALLED_SIZE@/$installed_size/" \
    "$input/packaging/control.in" >"$deb/DEBIAN/control"
chmod 0644 "$deb/DEBIAN/control"
find "$deb" -type d -exec chmod 0755 {} +
set_times "$deb"
dpkg-deb --root-owner-group -Zxz -z6 --build "$deb" "$output/$deb_name" >/dev/null

# ---- 3. AppImage: the type 2 runtime, then a SquashFS image of the AppDir
appdir="$work/AppDir"
mkdir -p "$appdir/usr/bin"
install -m 0755 "$input/packaging/AppRun" "$appdir/AppRun"
install -m 0755 "$input/taxbroker" "$input/taxbroker_server" "$appdir/usr/bin/"
install -m 0644 "$input/LICENSE" "$input/THIRD_PARTY_NOTICES.txt" "$appdir/usr/bin/"
install -m 0644 "$input/packaging/APPIMAGE_RUNTIME_NOTICES.txt" "$appdir/"
install -m 0644 "$work/README.txt" "$appdir/README.txt"
install -m 0644 "$work/taxbroker.png" "$appdir/taxbroker.png"
ln -s taxbroker.png "$appdir/.DirIcon"
sed 's/^Exec=.*/Exec=AppRun/' "$input/packaging/taxbroker.desktop" >"$appdir/taxbroker.desktop"
chmod 0644 "$appdir/taxbroker.desktop"
find "$appdir" -type d -exec chmod 0755 {} +
set_times "$appdir"
# mksquashfs takes every time (the image and each file) from SOURCE_DATE_EPOCH, and refuses
# explicit time options together with it.
mksquashfs "$appdir" "$work/appimage.squashfs" -noappend -quiet -no-progress -comp zstd \
    -Xcompression-level 15 -all-root -no-xattrs -processors 1
cat "$input/runtime-x86_64" "$work/appimage.squashfs" >"$output/$appimage_name"
chmod 0755 "$output/$appimage_name"

# ---- 4. checks of the entry and the sums
desktop-file-validate "$deb/usr/share/applications/taxbroker.desktop"
desktop-file-validate "$appdir/taxbroker.desktop"

(cd "$output" && sha256sum "$tar_name" "$deb_name" "$appimage_name" >SHA256SUMS)
echo "Made $tar_name, $deb_name, $appimage_name and SHA256SUMS in $output."
