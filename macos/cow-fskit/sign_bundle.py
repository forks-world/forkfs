"""Sign a disposable app copy; never install or modify the build output."""
import argparse
import shutil
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--app', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--identity', required=True)
args = parser.parse_args()
if args.identity == '-' or not args.identity.strip():
    parser.error('provide a real signing identity; ad-hoc signing is not an installation proof')
source = Path(__file__).resolve().parent
subprocess.run(['python3', str(source / 'check_bundle.py'), str(args.app), str(source)], check=True)
# copytree refuses an existing output; a failed signing attempt stays separate.
shutil.copytree(args.app, args.output, symlinks=True)
extension = args.output / 'Contents/PlugIns/ForkRevisionExtension.appex'
for bundle, entitlement in [(extension, 'Extension.entitlements'), (args.output, 'Host.entitlements')]:
    subprocess.run(['codesign', '--force', '--options', 'runtime', '--sign', args.identity,
                    '--entitlements', str(source / entitlement), str(bundle)], check=True)
subprocess.run(['codesign', '--verify', '--deep', '--strict', str(args.output)], check=True)
print(f'Signed and verified: {args.output}. Registration, provisioning and mounting remain unverified.')
