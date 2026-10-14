#!/bin/bash
set -eu
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ -f "$PROJECT_DIR/.local-build.env" ]]; then source "$PROJECT_DIR/.local-build.env"; fi
QT_ROOT="${QT_ROOT:-$HOME/Qt/6.11.2/macos}"
QT_TOOLS="${QT_TOOLS:-$HOME/Qt/Tools}"
BUILD_CMAKE="${BUILD_CMAKE:-$QT_TOOLS/CMake/CMake.app/Contents/bin/cmake}"
BUILD_NINJA="${BUILD_NINJA:-$QT_TOOLS/Ninja/ninja}"
SERVER_CONFIG=""
if [[ -f "$PROJECT_DIR/config/linkdesk-server.json" ]]; then
    SERVER_CONFIG="$PROJECT_DIR/config/linkdesk-server.json"
fi
"$BUILD_CMAKE" -S "$PROJECT_DIR/ForMac" -B "$PROJECT_DIR/ForMac/build" -G Ninja \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" -DCMAKE_MAKE_PROGRAM="$BUILD_NINJA" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DLINKDESK_SERVER_CONFIG="$SERVER_CONFIG"
"$BUILD_CMAKE" --build "$PROJECT_DIR/ForMac/build" -j 6
"$QT_TOOLS/CMake/CMake.app/Contents/bin/ctest" --test-dir "$PROJECT_DIR/ForMac/build" --output-on-failure
open "$PROJECT_DIR/ForMac/build/LinkDesk.app"
