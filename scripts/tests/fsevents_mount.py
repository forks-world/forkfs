#!/usr/bin/env python3
"""Qualify a live recursive FSEvents file stream on APFS or a private ForkFS mount."""
import argparse
import json
import os
from pathlib import Path
import platform
import select
import shutil
import stat
import subprocess
import sys
import tempfile

from filesystem_compat import check, filesystem

HELPER = r'''
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void events(ConstFSEventStreamRef stream, void *context, size_t count,
                   void *paths, const FSEventStreamEventFlags flags[],
                   const FSEventStreamEventId ids[]) {
    char **names = paths;
    (void)stream; (void)context; (void)ids;
    for (size_t i = 0; i < count; ++i)
        printf("%u\t%s\n", (unsigned)flags[i], names[i]);
    fflush(stdout);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    if (!path) return 3;
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&path, 1, &kCFTypeArrayCallBacks);
    CFRelease(path);
    if (!paths) return 3;
    FSEventStreamRef stream = FSEventStreamCreate(NULL, events, NULL, paths,
        kFSEventStreamEventIdSinceNow, 0.1,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!stream) return 4;
    dispatch_queue_t queue = dispatch_queue_create("forkfs.fsevents.probe", DISPATCH_QUEUE_SERIAL);
    if (!queue) { FSEventStreamInvalidate(stream); FSEventStreamRelease(stream); return 6; }
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream)) {
        FSEventStreamInvalidate(stream); FSEventStreamRelease(stream); dispatch_release(queue); return 5;
    }
    puts("READY"); fflush(stdout);
    sleep(9);
    FSEventStreamFlushSync(stream);
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    dispatch_release(queue);
    return 0;
}
'''
MUTATE = r'''
import os, pathlib, sys, time
root = pathlib.Path(sys.argv[1])
path = root / 'nested' / 'original'
renamed = path.with_name('renamed')
with path.open('xb'):
    pass
time.sleep(0.7)
with path.open('ab') as stream:
    stream.write(b'FSEvents data'); stream.flush(); os.fsync(stream.fileno())
time.sleep(0.7)
os.rename(path, renamed)
time.sleep(0.7)
os.unlink(renamed)
'''
# Public FSEvents item flags; events can coalesce and arrive in any order.
FLAGS = {'created': 0x00000100, 'removed': 0x00000200,
         'renamed': 0x00000800, 'modified': 0x00001000}


def run(target, kind):
    report = {'schema': 1, 'platform': platform.platform(), 'filesystem': kind,
              'status': 'failed', 'setup': 'not_started', 'events': [],
              'limits': ['live file events in one private subtree', 'no ordering or event count guarantee',
                         'no history replay, overflow recovery or revoke qualification']}
    scratch = Path(tempfile.mkdtemp(prefix='forkfs-fsevents-', dir=target))
    process = None
    try:
        nested = scratch / 'nested'
        nested.mkdir()
        with tempfile.TemporaryDirectory(prefix='forkfs-fsevents-helper-') as build:
            source = Path(build) / 'probe.c'
            binary = Path(build) / 'probe'
            source.write_text(HELPER)
            compiled = subprocess.run(['xcrun', 'clang', str(source), '-framework', 'CoreServices',
                                       '-o', str(binary)], capture_output=True, text=True, timeout=30)
            check(compiled.returncode == 0, 'helper compilation failed: ' + compiled.stderr.strip())
            process = subprocess.Popen([str(binary), str(scratch)], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, bufsize=1)
            check(select.select([process.stdout], [], [], 10)[0], 'stream setup deadline exceeded')
            check(process.stdout.readline().strip() == 'READY', 'stream setup failed')
            report['setup'] = 'started'
            mutation = subprocess.run([sys.executable, '-c', MUTATE, str(scratch)],
                                      capture_output=True, text=True, timeout=15)
            check(mutation.returncode == 0, 'mutation failed: ' + mutation.stderr.strip())
            report['mutation'] = 'passed'
            output, stderr = process.communicate(timeout=15)
            check(process.returncode == 0, 'stream helper failed: ' + stderr.strip())
            for line in output.splitlines():
                flag, path = line.split('\t', 1)
                report['events'].append({'path': path, 'flags': int(flag)})
            names = {str(nested / 'original'), str(nested / 'renamed')}
            observed = 0
            stream_loss = 0
            root_changed = False
            for event in report['events']:
                stream_loss |= event['flags'] & 0x7  # MustScanSubDirs, user/kernel dropped.
                root_changed |= bool(event['flags'] & 0x20)
                if event['path'] in names:
                    observed |= event['flags']
            report['stream_loss_flags'] = stream_loss
            report['root_changed'] = root_changed
            report['observed_item_flags'] = {name: bool(observed & bit) for name, bit in FLAGS.items()}
            report['missing'] = [name for name, seen in report['observed_item_flags'].items() if not seen]
            report['status'] = 'passed' if not report['missing'] and not stream_loss and not root_changed else 'failed'
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
    finally:
        if process and process.poll() is None:
            process.kill()
            process.communicate()
        shutil.rmtree(scratch)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    args = parser.parse_args()
    check(sys.platform == 'darwin', 'requires macOS')
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if not args.reference_apfs:
        st = target.stat()
        check(target == mounted and st.st_uid == os.geteuid() and stat.S_IMODE(st.st_mode) == 0o700,
              'requires owned private forkfs root')
    report = run(target, kind)
    print(json.dumps(report, indent=2))
    return int(report['status'] != 'passed')


if __name__ == '__main__':
    sys.exit(main())
