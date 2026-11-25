#!/usr/bin/env python3
"""Measure directory descriptor behavior on APFS or an owned forkfs mount."""
import argparse
import errno
import json
import os
from pathlib import Path
import stat
import shutil
import subprocess
import sys
import tempfile

from filesystem_compat import check, filesystem

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tests.fs_contract.adapters import _DarwinXattr

ATTR = 'user.forkfs-directory-fd'
ORIGINAL = b'original\x00value'
REPLACEMENT = b'replacement\x00value'


def worker(root, scenario):
    xattr = _DarwinXattr()
    results = []

    def probe(name, expected, action):
        try:
            value = action()
            observed = {'errno': 0, 'value': value}
        except OSError as exc:
            observed = {'errno': exc.errno, 'errno_name': errno.errorcode.get(exc.errno), 'message': str(exc)}
        results.append({'op': name, 'expected': expected, 'observed': observed,
                        'pass': observed.get('errno') == expected['errno'] and
                        ('value' not in expected or observed.get('value') == expected['value'])})
        return observed

    def identity(st):
        return {'dev': st.st_dev, 'ino': st.st_ino, 'directory': stat.S_ISDIR(st.st_mode)}

    source = root / 'source'
    target = root / 'target'
    if scenario == 'rename_replace':
        parent = root / 'other'
        parent.mkdir(mode=0o700)
        source = parent / 'target'
    source.mkdir(mode=0o700)
    xattr.set(source, ATTR, ORIGINAL, 'upsert')
    fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        initial = os.fstat(fd)
        expected_identity = identity(initial)
        if scenario == 'rmdir_reuse':
            os.rmdir(source)
            probe("removed_path", {"errno": errno.ENOENT}, lambda: identity(os.stat(source)))
            source.mkdir(mode=0o700)
            replacement = source
        elif scenario == 'rename_replace':
            replacement_source = root / 'replacement-source'
            replacement_source.mkdir(mode=0o700)
            replacement_identity = identity(os.stat(replacement_source))
            # The descriptor pins the destination replaced from another parent.
            os.rename(replacement_source, source)
            probe("replacement_source_path", {"errno": errno.ENOENT}, lambda: identity(os.stat(replacement_source)))
            probe("moved_replacement_identity", {"errno": 0, "value": replacement_identity}, lambda: identity(os.stat(source)))
            replacement = source
        else:
            os.rename(source, target)
            source.mkdir(mode=0o700)
            replacement = source
        xattr.set(replacement, ATTR, REPLACEMENT, 'upsert')
        replacement_initial = identity(os.stat(replacement))
        results.append({'op': 'replacement_identity', 'expected': {'different_from': expected_identity},
                        'observed': replacement_initial, 'pass': replacement_initial != expected_identity})
        def fd_stat():
            st = os.fstat(fd)
            value = identity(st)
            value['nlink'] = st.st_nlink
            return value
        observed = probe('fstat', {'errno': 0, 'identity': expected_identity}, fd_stat)
        if observed['errno'] == 0:
            results[-1]['pass'] = all(observed['value'][k] == v for k, v in expected_identity.items())
        probe('fd_list_xattrs', {'errno': 0, 'value': True}, lambda: ATTR in xattr.fd_list(fd))
        probe('fd_get_xattr', {'errno': 0, 'value': ORIGINAL.hex()}, lambda: xattr.fd_get(fd, ATTR).hex())
        detached = scenario != 'live_rename_reuse'
        if not detached:
            probe('moved_identity_before_mkdir', {'errno': 0, 'value': expected_identity}, lambda: identity(os.stat(target)))
        probe('mkdirat', {'errno': errno.ENOENT if detached else 0}, lambda: os.mkdir('child', 0o700, dir_fd=fd))
        probe('replacement_stat', {'errno': 0, 'value': replacement_initial}, lambda: identity(os.stat(replacement)))
        probe('replacement_xattr', {'errno': 0, 'value': REPLACEMENT.hex()}, lambda: xattr.get(replacement, ATTR).hex())
        probe('replacement_child', {'errno': errno.ENOENT}, lambda: identity(os.stat(replacement / 'child')))
        if not detached:
            probe('moved_identity_after_mkdir', {'errno': 0, 'value': expected_identity}, lambda: identity(os.stat(target)))
            probe('moved_child', {'errno': 0, 'value': True}, lambda: stat.S_ISDIR(os.stat(target / 'child').st_mode))
    finally:
        os.close(fd)
    return {'scenario': scenario, 'operations': results, 'pass': all(item['pass'] for item in results)}


def run_probe(mountpoint, reference):
    try:
        check(sys.platform == 'darwin', 'requires macOS')
        target = mountpoint.resolve(strict=True)
        mounted, kind = filesystem(target)
        check(kind == ('apfs' if reference else 'forkfs'), 'unexpected containing filesystem: ' + kind)
        st = target.stat()
        if not reference:
            check(mountpoint.absolute() == target == mounted and st.st_uid == os.geteuid() and
                  stat.S_IMODE(st.st_mode) == 0o700, 'candidate requires exact owned mode-0700 forkfs root')
    except (OSError, RuntimeError) as exc:
        return {'pass': False, 'cleanup_pending': False, 'error': str(exc)}
    scratch = Path(tempfile.mkdtemp(prefix='forkfs-directory-fd-', dir=target))
    scratch_st = scratch.lstat()
    report = {'filesystem': kind, 'scratch': str(scratch), 'scenarios': [], 'cleaned': False, 'cleanup_pending': True}
    for name in ('rmdir_reuse', 'rename_replace', 'live_rename_reuse'):
        try:
            case = scratch / name
            case.mkdir(mode=0o700)
            report['scenarios'].append(worker(case, name))
        except Exception as exc:
            report['scenarios'].append({'scenario': name, 'pass': False, 'error': f'{type(exc).__name__}: {exc}'})
    try:
        now = target.stat(); current = scratch.lstat()
        check(filesystem(target) == (mounted, kind) and (now.st_dev, now.st_ino) == (st.st_dev, st.st_ino), 'target or filesystem changed')
        if not reference:
            check(now.st_uid == os.geteuid() and stat.S_IMODE(now.st_mode) == 0o700, 'forkfs root changed ownership or mode')
        check(scratch.parent == target and scratch.name.startswith('forkfs-directory-fd-') and
              stat.S_ISDIR(current.st_mode) and current.st_uid == os.geteuid() and stat.S_IMODE(current.st_mode) == 0o700 and
              (current.st_dev, current.st_ino) == (scratch_st.st_dev, scratch_st.st_ino), 'scratch identity or ownership changed')
        shutil.rmtree(scratch)
        report['cleaned'] = True
        report['cleanup_pending'] = False
    except Exception as exc:
        report['cleanup_error'] = f'{type(exc).__name__}: {exc}'
    report['pass'] = report['cleaned'] and len(report['scenarios']) == 3 and all(case['pass'] for case in report['scenarios'])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        try:
            report = run_probe(args.mountpoint, args.reference_apfs)
        except Exception as exc:
            report = {'pass': False, 'cleanup_pending': True, 'error': f'{type(exc).__name__}: {exc}'}
        print(json.dumps(report, sort_keys=True))
        return 0
    cmd = [sys.executable, str(Path(__file__).resolve()), str(args.mountpoint), '--worker']
    if args.reference_apfs: cmd.append('--reference-apfs')
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        check(proc.returncode == 0, 'worker failed: ' + proc.stderr.strip())
        report = json.loads(proc.stdout)
        check(isinstance(report, dict) and isinstance(report.get('pass'), bool), 'invalid worker report')
    except subprocess.TimeoutExpired:
        report = {'pass': False, 'cleanup_pending': True, 'error': 'worker timed out; scratch location unknown'}
    except (OSError, ValueError, RuntimeError) as exc:
        report = {'pass': False, 'cleanup_pending': True, 'error': str(exc)}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
