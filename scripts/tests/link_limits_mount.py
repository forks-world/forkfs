#!/usr/bin/env python3
"""Qualify native hardlink EMLINK and capacity reuse on an owned fixture.

forkfs requires a disposable repository made by forkfsd_link_limits_test --fixture.
APFS creates/removes its own scratch fixture. Forkfs mutations are restored.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import tempfile
from filesystem_compat import check, filesystem

LIMIT=32767


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mountpoint',type=Path)
    parser.add_argument('--reference-apfs',action='store_true');parser.add_argument('--readonly-fixture',action='store_true')
    args=parser.parse_args();target=args.mountpoint.resolve(strict=True);mounted,kind=filesystem(target)
    check(kind==('apfs' if args.reference_apfs else 'forkfs'),'unexpected filesystem')
    if not args.reference_apfs:check(target==mounted and target.stat().st_mode&0o777==0o700,'requires private forkfs root')
    root=Path(tempfile.mkdtemp(prefix='forkfs-link-limit-',dir=target)) if args.reference_apfs else target
    path=root/'file';links=root/'links';reused=links/'reused';first=links/'n000000'
    try:
        if args.reference_apfs:
            check(not args.readonly_fixture,'APFS readonly fixture is not supplied');path.write_bytes(b'D');links.mkdir()
            for i in range(LIMIT-1):os.link(path,links/('n%06d'%i))
        check(path.stat().st_nlink==LIMIT and path.read_bytes()==b'D','fixture mismatch')
        check(first.stat().st_ino==path.stat().st_ino and first.stat().st_nlink==LIMIT,'alias identity mismatch')
        advertised=os.pathconf(path,'PC_LINK_MAX');check(advertised==LIMIT,'advertised link maximum differs')
        if args.readonly_fixture:
            check(not reused.exists(),'revision namespace changed')
            print(json.dumps({'readonly_maximum_links_after_gc':LIMIT,'advertised_link_max':advertised,'identity_and_data_preserved':True}));return
        def overflow():
            try:os.link(path,reused)
            except OSError as error:check(error.errno==errno.EMLINK,'unexpected full-link errno %s'%error.errno);return error.errno
            check(args.reference_apfs,'link beyond advertised maximum succeeded')
            reused.unlink();return None
        code=overflow();check(not reused.exists() and path.stat().st_nlink==LIMIT,'failed link changed state')
        first.unlink();check(path.stat().st_nlink==LIMIT-1,'unlink did not release one link')
        os.link(path,reused);check(reused.stat().st_ino==path.stat().st_ino and path.stat().st_nlink==LIMIT,'capacity reuse failed')
        reused.unlink();os.link(path,first)
        check(path.stat().st_nlink==LIMIT and path.read_bytes()==b'D','restored fixture changed')
        print(json.dumps({'filesystem':kind,'advertised_link_max':advertised,'links':LIMIT,'overflow_errno':code,'allows_beyond_advertised':code is None,
                          'overflow_probe_preserves_state':True,'unlink_reuses_capacity':True,'fixture_restored':True},indent=2))
    finally:
        if args.reference_apfs:shutil.rmtree(root)


if __name__=='__main__':main()
