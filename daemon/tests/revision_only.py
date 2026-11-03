"""The supported build cannot opt back into the native-directory runtime."""
from pathlib import Path
import subprocess
import sys
import tempfile
source, build = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='ff-retired-', dir='/tmp') as tmp:
    for option in ['WFS_BUILD_LEGACY']:
        result = subprocess.run(['cmake', '-S', str(source), '-B', str(Path(tmp)/option),
                                 '-D'+option+'=ON'], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0, option
        assert 'backend is retired' in result.stdout + result.stderr, (option, result.stdout, result.stderr)
    stale = Path(tmp)/'stale'
    (stale/'cli').mkdir(parents=True)
    binary=stale/'cli/world'
    binary.write_bytes(b'existing-user-artifact')
    result = subprocess.run(['cmake', '-S', str(source), '-B', str(stale)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and 'retired world' in result.stdout+result.stderr
    assert binary.read_bytes()==b'existing-user-artifact'
assert not (build/'cli/world').exists()
assert not (build/'core').exists()
print('Native runtime switches and stale build reuse are rejected; daemon-only build verified')
