#!/bin/bash
# Build with CMake, assemble WorldFS.app + FSKit appex, ad-hoc sign, install to ~/Applications, register.
# No Xcode required. Usage: scripts/bundle.sh [Release|Debug]
set -euo pipefail
cd "$(dirname "$0")/.."
CONFIG=${1:-Release}
BUILD=build/$CONFIG
APP=build/WorldFS.app
APPEX=$APP/Contents/Extensions/WorldFSExtension.appex

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE="$CONFIG" >/dev/null
cmake --build "$BUILD" --parallel
(cd "$BUILD" && ctest --output-on-failure)

rm -rf "$APP"
mkdir -p "$APPEX/Contents/MacOS" "$APP/Contents/MacOS"
cp "$BUILD/macos/fskit/WorldFSExtension" "$APPEX/Contents/MacOS/WorldFSExtension"
cp macos/fskit/Info.plist "$APPEX/Contents/Info.plist"
cp macos/fskit/App-Info.plist "$APP/Contents/Info.plist"
cp "$BUILD/cli/world" "$APP/Contents/MacOS/WorldFS"   # host app binary = the CLI

codesign --force --sign - --entitlements macos/fskit/WorldFSExtension.entitlements "$APPEX"
codesign --force --sign - "$APP"

# Install where LaunchServices scans, and register. The FSKit enable switch itself lives in
# System Settings > General > Login Items & Extensions > File System Extensions (entitlement-gated).
# Update in place (rsync) rather than rm -rf, so LaunchServices keeps the same bundle record and the
# enable switch survives rebuilds. macOS registers an app's extensions when the app is launched, so
# launch it once, hidden, after every install.
INSTALL=$HOME/Applications/WorldFS.app
mkdir -p "$HOME/Applications"
rsync -a --delete "$APP/" "$INSTALL/"
LSREG=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
$LSREG -u "$APP" >/dev/null 2>&1 || true      # never leave the build/ copy registered
$LSREG -f -R "$INSTALL"
open -g -j "$INSTALL"
sleep 2
echo "installed: $INSTALL"
echo "cli: $BUILD/cli/world"
"$BUILD/cli/world" fs status || true
