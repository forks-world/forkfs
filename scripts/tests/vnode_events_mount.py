#!/usr/bin/env python3
"""Qualify live kqueue vnode notifications on a private mount or APFS reference.

Events may coalesce: tests require flag coverage, never event counts/order.
Does not qualify FSEvents, recursive watches, revoke or historical replay.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import select
import shutil
import subprocess
import sys
import tempfile
import time
from filesystem_compat import check, filesystem

BITS = ('WRITE', 'EXTEND', 'ATTRIB', 'LINK', 'RENAME', 'DELETE')


class Watch:
    def __init__(self, fd):
        self.fd = fd; self.queue = select.kqueue()
        mask = sum(getattr(select, 'KQ_NOTE_'+name) for name in BITS)
        event = select.kevent(fd, filter=select.KQ_FILTER_VNODE,
                             flags=select.KQ_EV_ADD|select.KQ_EV_CLEAR, fflags=mask)
        self.queue.control([event], 0, 0)

    def drain(self, timeout=0):
        result = 0
        for event in self.queue.control(None, 16, timeout):
            check(event.ident == self.fd and event.filter == select.KQ_FILTER_VNODE, 'wrong watched identity')
            check(not event.flags & select.KQ_EV_ERROR, 'kqueue event error')
            result |= event.fflags
        return result

    def expect(self, operation, *names):
        self.drain(); operation(); expected = sum(getattr(select, 'KQ_NOTE_'+name) for name in names)
        seen = 0; end = time.monotonic()+3
        while seen & expected != expected and time.monotonic() < end:
            seen |= self.drain(max(0, end-time.monotonic()))
        check(seen & expected == expected, 'missing %s, got flags %s' % (names, seen))
        return [name for name in BITS if seen & getattr(select, 'KQ_NOTE_'+name)]

    def close(self):
        self.queue.close()


def file_events(root):
    path=root/'file'; path.write_bytes(b'base'); fd=os.open(path, os.O_RDWR)
    watch=Watch(fd); report={}
    try:
        def append_child():
            code='import os,sys; fd=os.open(sys.argv[1],os.O_WRONLY|os.O_APPEND); os.write(fd,b"child"); os.fsync(fd); os.close(fd)'
            # Keep interpreter/xcrun startup caches off the filesystem under test.
            # Only the explicitly opened file should exercise mounted append I/O.
            with tempfile.TemporaryDirectory(prefix='forkfs-vnode-child-', dir='/private/tmp') as child_home:
                subprocess.run([sys.executable, '-c', code, str(path)], check=True, capture_output=True, timeout=15,
                               env={'PATH':'/usr/bin:/bin','HOME':child_home,'TMPDIR':child_home})
        report['cross_process_append']=watch.expect(append_child,'WRITE','EXTEND')
        check(os.pread(fd,9,0)==b'basechild','event preceded incorrect file content')
        report['overwrite']=watch.expect(lambda: os.pwrite(fd,b'B',0),'WRITE')
        report['chmod']=watch.expect(lambda: os.chmod(path,0o400),'ATTRIB')
        alias=root/'alias';report['link']=watch.expect(lambda: os.link(path,alias),'LINK')
        check(os.fstat(fd).st_nlink==2,'link notification without metadata change')
        report['unlink_alias']=watch.expect(alias.unlink,'LINK')
        renamed=root/'renamed';report['rename']=watch.expect(lambda:path.rename(renamed),'RENAME')
        report['unlink']=watch.expect(renamed.unlink,'DELETE')
        watch.drain();renamed.write_bytes(b'replacement')
        stale=watch.drain(0.2)
        check(not stale & (select.KQ_NOTE_WRITE|select.KQ_NOTE_EXTEND), 'old watch followed reused pathname')
        report['orphan_write']=watch.expect(lambda:os.pwrite(fd,b'ORPHAN',12),'WRITE','EXTEND')
        check(renamed.read_bytes()==b'replacement' and os.fstat(fd).st_nlink==0,'orphan watch lost inode identity')
        report['path_reuse_isolated']=True
    finally:
        watch.close();os.close(fd)
    return report


def directory_events(root):
    fd=os.open(root,os.O_RDONLY);watch=Watch(fd);report={}
    try:
        path=root/'created';report['create']=watch.expect(lambda:path.write_bytes(b'data'),'WRITE')
        renamed=root/'renamed';report['rename']=watch.expect(lambda:path.rename(renamed),'WRITE')
        report['unlink']=watch.expect(renamed.unlink,'WRITE')
        child=root/'child';report['mkdir']=watch.expect(child.mkdir,'WRITE')
        report['rmdir']=watch.expect(child.rmdir,'WRITE')
    finally:
        watch.close();os.close(fd)
    return report


CASES={'file-events':file_events,'directory-events':directory_events}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint',type=Path)
    parser.add_argument('--reference-apfs',action='store_true')
    parser.add_argument('--worker',choices=CASES,help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.worker:
        print(json.dumps(CASES[args.worker](args.mountpoint)));return 0
    check(sys.platform=='darwin','requires macOS kqueue')
    target=args.mountpoint.resolve(strict=True);mounted,kind=filesystem(target)
    check(kind==('apfs' if args.reference_apfs else 'forkfs'),'unexpected filesystem')
    if not args.reference_apfs:
        check(target==mounted and target.stat().st_mode&0o777==0o700,'requires private forkfs root')
    root=Path(tempfile.mkdtemp(prefix='forkfs-vnode-',dir=target))
    report={'schema':1,'platform':platform.platform(),'filesystem':kind,'cases':{},
            'limits':['live vnode watches only','events may coalesce','no FSEvents, recursive watch, revoke or revision replay qualification']}
    try:
        for name in CASES:
            case_root=root/name;case_root.mkdir(mode=0o700)
            try:
                result=subprocess.run([sys.executable,str(Path(__file__).resolve()),str(case_root),'--worker',name],
                                      capture_output=True,text=True,timeout=60)
                check(result.returncode==0,result.stderr.strip() or 'event worker failed')
                report['cases'][name]={'status':'passed','details':json.loads(result.stdout)}
            except (RuntimeError,subprocess.TimeoutExpired) as error:
                report['cases'][name]={'status':'failed','error':str(error)}
    finally:
        shutil.rmtree(root)
    print(json.dumps(report,indent=2))
    return int(any(case['status']!='passed' for case in report['cases'].values()))


if __name__=='__main__':
    sys.exit(main())
