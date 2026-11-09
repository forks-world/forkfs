#!/bin/bash
# Build and sign a reviewable app bundle. Does not install, register, enable or mount.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/forkfs}
: "${FORKFS_SIGN_IDENTITY:?Set FORKFS_SIGN_IDENTITY to an Apple signing identity}"
: "${FORKFS_MODULE_PROFILE:?Set FORKFS_MODULE_PROFILE to the FSKit provisioning profile}"
if [[ "$FORKFS_SIGN_IDENTITY" == - || ! -f "$FORKFS_MODULE_PROFILE" ]]; then
    echo "FSKit requires an Apple signing identity and a provisioning profile." >&2
    exit 1
fi
SIGNING_TMP=$(mktemp -d "${TMPDIR:-/tmp}/forkfs-signing.XXXXXX")
trap 'rm -rf "$SIGNING_TMP"' EXIT
security cms -D -i "$FORKFS_MODULE_PROFILE" -o "$SIGNING_TMP/profile.plist"
python3 - "$SIGNING_TMP/profile.plist" macos/forkfs/ForkFSExtension.entitlements "$SIGNING_TMP/extension.entitlements" <<'PYCODE'
import datetime, plistlib, sys
with open(sys.argv[1], 'rb') as stream: profile = plistlib.load(stream)
permissions = profile['Entitlements']
team, = profile['TeamIdentifier']
app_id = team + '.world.forks.forkfs.extension'
if permissions.get('com.apple.developer.fskit.fsmodule') is not True:
    raise SystemExit('Profile does not authorize FSKit Module')
if permissions.get('com.apple.application-identifier') != app_id:
    raise SystemExit('Profile does not match the ForkFS extension bundle ID')
if profile['ExpirationDate'] <= datetime.datetime.utcnow():
    raise SystemExit('Profile has expired')
with open(sys.argv[2], 'rb') as stream: entitlements = plistlib.load(stream)
entitlements['com.apple.application-identifier'] = app_id
entitlements['com.apple.developer.team-identifier'] = team
with open(sys.argv[3], 'wb') as stream: plistlib.dump(entitlements, stream)
PYCODE
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DWFS_FSKIT=ON
cmake --build "$BUILD" --target ForkFSExtension ForkFSHost forkfsd --parallel 4
APP="$BUILD/ForkFS.app"
APPEX="$APP/Contents/Extensions/ForkFSExtension.appex"
mkdir -p "$APPEX/Contents/MacOS" "$APP/Contents/MacOS"
cp "$BUILD/macos/forkfs/ForkFSExtension" "$APPEX/Contents/MacOS/ForkFSExtension"
cp "$BUILD/macos/forkfs/ForkFSHost" "$APP/Contents/MacOS/ForkFSHost"
cp macos/forkfs/Info.plist "$APPEX/Contents/Info.plist"
cp macos/forkfs/App-Info.plist "$APP/Contents/Info.plist"
cp "$FORKFS_MODULE_PROFILE" "$APPEX/Contents/embedded.provisionprofile"
codesign --force --sign "$FORKFS_SIGN_IDENTITY" --timestamp=none --entitlements "$SIGNING_TMP/extension.entitlements" "$APPEX"
codesign -d --extract-certificates="$SIGNING_TMP/cert" "$APPEX"
python3 - "$SIGNING_TMP/profile.plist" "$SIGNING_TMP/cert0" <<'PYCODE'
import plistlib, sys
with open(sys.argv[1], 'rb') as stream: profile = plistlib.load(stream)
with open(sys.argv[2], 'rb') as stream: certificate = stream.read()
if certificate not in profile['DeveloperCertificates']:
    raise SystemExit('Signing certificate is not authorized by the FSKit profile')
PYCODE
codesign --force --sign "$FORKFS_SIGN_IDENTITY" --timestamp=none "$APP"
codesign --verify --deep --strict "$APP"
echo "bundle: $APP"
echo "daemon: $BUILD/daemon/forkfsd"
