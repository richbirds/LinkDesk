#!/bin/bash
set -eu
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ -f "$PROJECT_DIR/.local-build.env" ]]; then source "$PROJECT_DIR/.local-build.env"; fi
QT_ROOT="${QT_ROOT:-$HOME/Qt/6.11.2/macos}"
cd "$PROJECT_DIR"
mkdir -p ForMac/dist/archive
if [[ -d ForMac/dist/LinkDesk.app ]]; then
    ARCHIVE_NAME="LinkDesk-$(TZ=Asia/Kuala_Lumpur date +%Y%m%d-%H%M%S).app"
    mv ForMac/dist/LinkDesk.app "ForMac/dist/archive/$ARCHIVE_NAME"
fi
ditto ForMac/build/LinkDesk.app ForMac/dist/LinkDesk.app
"$QT_ROOT/bin/macdeployqt" ForMac/dist/LinkDesk.app -verbose=1
mkdir -p ForMac/dist/LinkDesk.app/Contents/Resources/licenses
cp -R third_party/qt ForMac/dist/LinkDesk.app/Contents/Resources/licenses/Qt
codesign --force --deep --sign - ForMac/dist/LinkDesk.app
codesign --verify --deep --strict ForMac/dist/LinkDesk.app
ditto -c -k --sequesterRsrc --keepParent ForMac/dist/LinkDesk.app ForMac/dist/LinkDesk-Mac-arm64.zip
echo "已生成 ForMac/dist/LinkDesk.app 与 ForMac/dist/LinkDesk-Mac-arm64.zip"
