"""Validate unsigned build layout; this does not verify signing or registration."""
import plistlib
import sys
from pathlib import Path

app = Path(sys.argv[1])
source = Path(sys.argv[2])

def plist(path):
    with path.open('rb') as stream:
        return plistlib.load(stream)

def check_bundle(bundle, identifier, executable):
    info = plist(bundle / 'Contents/Info.plist')
    assert info['CFBundleIdentifier'] == identifier
    assert info['CFBundleExecutable'] == executable
    binary = bundle / 'Contents/MacOS' / executable
    assert binary.is_file() and not binary.is_symlink()
    with binary.open('rb') as stream:
        assert stream.read(4) in [b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe']
    return info

check_bundle(app, 'world.forks.cow.revision', 'ForkfsRevision')
extension = app / 'Contents/PlugIns/ForkRevisionExtension.appex'
info = check_bundle(extension, 'world.forks.cow.revision.extension', 'ForkRevisionExtension')
attributes = info['EXAppExtensionAttributes']
assert attributes['EXExtensionPrincipalClass'] == 'ForkRevisionFileSystem'
assert attributes['FSShortName'] == 'forkrevision'
assert attributes['FSRequiresSecurityScopedPathURLResources'] is True
assert plist(source / 'Extension.entitlements')['com.apple.security.app-sandbox'] is True
host = plist(source / 'Host.entitlements')
assert host['com.apple.security.app-sandbox'] is True
assert host['com.apple.security.files.user-selected.read-only'] is True
print('unsigned host/extension layout and entitlement declarations verified')
