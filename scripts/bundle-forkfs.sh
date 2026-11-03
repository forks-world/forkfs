#!/bin/bash
# Build and sign a reviewable app bundle. Does not install, register, enable or mount.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/forkfs}
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DWFS_FSKIT=ON
cmake --build "$BUILD" --target ForkFSExtension ForkFSHost forkfsd --parallel 4
APP="$BUILD/ForkFS.app"
APPEX="$APP/Contents/Extensions/ForkFSExtension.appex"
mkdir -p "$APPEX/Contents/MacOS" "$APP/Contents/MacOS"
cp "$BUILD/macos/forkfs/ForkFSExtension" "$APPEX/Contents/MacOS/ForkFSExtension"
cp "$BUILD/macos/forkfs/ForkFSHost" "$APP/Contents/MacOS/ForkFSHost"
cp macos/forkfs/Info.plist "$APPEX/Contents/Info.plist"
cp macos/forkfs/App-Info.plist "$APP/Contents/Info.plist"
codesign --force --sign - --entitlements macos/forkfs/ForkFSExtension.entitlements "$APPEX"
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP"
echo "bundle: $APP"
echo "daemon: $BUILD/daemon/forkfsd"
