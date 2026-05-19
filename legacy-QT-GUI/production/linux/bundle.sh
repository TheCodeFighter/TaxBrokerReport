#!/bin/bash
set -e

VERSION=${1:-1.0.0}
OUT_DIR="/export"
ICON_PATH="/app/resources/gui/icons/qt_logo.png"

echo "📦 Bundling EdavkiXmlMaker v$VERSION..."

# 1. Environment Overdrive
export PATH="/tools:/usr/lib/qt6/bin:$PATH"
export QMAKE="/usr/lib/qt6/bin/qmake"
export QT_QPA_PLATFORM=offscreen
export EXTRA_QT_PLUGINS="platforms;xcbglintegrations"
export APPIMAGE_EXTRACT_AND_RUN=1

# 2. Tool Preparation
cd /tools
if [ ! -d "squashfs-root" ]; then
    echo "Extracting linuxdeploy..."
    ./linuxdeploy-x86_64.AppImage --appimage-extract > /dev/null
fi
chmod +x linuxdeploy-plugin-qt-x86_64.AppImage

# 3. Icon Safety (The usual suspect)
if [ ! -f "$ICON_PATH" ]; then
    echo "Icon missing. Creating fallback..."
    mkdir -p "$(dirname "$ICON_PATH")"
    echo "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=" | base64 -d > "$ICON_PATH"
fi

# 4. Execution
echo "🏗️  Manufacturing AppImage..."
rm -rf /tmp/appdir # Clean slate

REAL_EXEC_PATH="/app/build_prod/EdavkiXmlMaker"

./squashfs-root/AppRun \
    --appdir /tmp/appdir \
    --executable "$REAL_EXEC_PATH" \
    --desktop-file /app/production/linux/EdavkiXmlMaker.desktop \
    --icon-file "$ICON_PATH" \
    --plugin qt \
    --output appimage

# 5. Delivery
GENERATED=$(ls EdavkiXmlMaker-*.AppImage 2>/dev/null | head -n 1)

if [ -f "$GENERATED" ]; then
    mv "$GENERATED" "$OUT_DIR/EdavkiXmlMaker-v$VERSION-x86_64.AppImage"
    echo "🚀 Success! Payload delivered to $OUT_DIR"
else
    echo "Error: Factory output was empty."
    exit 1
fi