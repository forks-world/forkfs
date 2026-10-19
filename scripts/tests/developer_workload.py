"""Seeded resident-RPC development regression/stress workload (not mounted I/O).

Every run owns a fresh private artifact directory. Failures retain the repository,
seed, operation trace and metrics. No mount, shell, network or user tree is used.
"""
import argparse
import concurrent.futures
import copy
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import random
import platform
import signal
import socket
import statistics
import struct
import subprocess
import tempfile
import threading
import time
import traceback


MUTATIONS = {'fs-init', 'fs-mkdir', 'fs-rmdir', 'fs-write', 'fs-save', 'fs-write-at',
             'fs-append', 'fs-truncate', 'fs-rename', 'fs-unlink', 'fs-link',
             'fs-symlink', 'fs-chmod', 'fs-snapshot', 'fs-fork', 'fs-restore', 'fs-hwrite'}


@dataclass
class Node:
    kind: str
    data: bytes = b''
    mode: int = 0o644


def distribution(samples):
    values = sorted(samples)
    return {'count': len(values), 'p50_ms': statistics.median(values) / 1e6,
            'p95_ms': values[(95 * len(values) + 99) // 100 - 1] / 1e6 if len(values) >= 1000 else None,
            'p95_status': 'reported' if len(values) >= 1000 else 'insufficient_samples',
            'max_ms': values[-1] / 1e6}


def check_recovered_save(old, new, actual, acknowledged):
    assert actual in (old, new), 'in-flight save recovered torn contents'
    if acknowledged:
        assert actual == new, 'acknowledged save lost after process crash'


def disk_usage(path):
    result = dict(apparent_bytes=0, allocated_bytes=0, segment_bytes=0, index_bytes=0, files=0)
    for item in path.rglob('*'):
        if item.is_file():
            st = item.stat()
            result['apparent_bytes'] += st.st_size
            result['allocated_bytes'] += st.st_blocks * 512
            result['segment_bytes' if item.name.startswith('DATA') else 'index_bytes'] += st.st_size
            result['files'] += 1
    return result


class Workload:
    def __init__(self, args):
        self.args = args
        args.output.mkdir(parents=True, exist_ok=True)
        # Short private socket path independent of the artifact path length.
        self.runtime = Path(tempfile.mkdtemp(prefix='ff-dev-'))
        self.root = Path(tempfile.mkdtemp(prefix=f'seed-{args.seed}-', dir=args.output)).resolve()
        self.db, self.endpoint = self.root / 'repository', self.runtime / 'rpc.sock'
        self.binary = str(args.binary.resolve())
        self.proc = None
        self.lock = threading.Lock()
        self.samples, self.storage, self.coverage = {}, [], {}
        self.phase_samples, self.phase = {}, "startup"
        self.outcomes, self.barrier_crashes = {}, 0
        self.started = time.monotonic()
        self.worlds = {'main': {'/': Node('dir', mode=0o755)}}
        self.revisions = {}
        self.probes = set()
        self.input_bytes = self.mutations = self.crash_trials = 0
        self.log = (self.root / 'operations.jsonl').open('w', buffering=1)
        self.daemon_log = (self.root / 'daemon.log').open('ab')
        self.settings = vars(args) | {'binary': self.binary, 'pid': os.getpid(),
                                      'binary_sha256': hashlib.sha256(Path(self.binary).read_bytes()).hexdigest(),
                                      'platform': platform.platform(), 'python': platform.python_version(),
                                      'transport': 'resident Unix RPC; one connection per operation',
                                      'cache': 'OS caches not flushed; service restarts reset daemon caches',
                                      'scope': 'synthetic development I/O; no mounted git/compiler execution'}
        (self.root / 'settings.json').write_text(json.dumps(self.settings, default=str, indent=2))
        print(f'Artifacts: {self.root}', flush=True)

    def cli(self, *args):
        p = subprocess.run([self.binary, *map(str, args)], capture_output=True, timeout=120)
        if p.returncode:
            raise AssertionError((args, p.returncode, p.stderr.decode(errors='replace')))
        return p.stdout

    def start(self, db=None):
        self.endpoint.unlink(missing_ok=True)
        self.proc = subprocess.Popen([self.binary, 'serve', str(db or self.db), '--socket',
                                      str(self.endpoint), '--rpc'], stdout=self.daemon_log, stderr=self.daemon_log)
        deadline = time.monotonic() + 30
        while True:
            if self.proc.poll() is not None:
                raise AssertionError('daemon exited during startup; see daemon.log')
            if time.monotonic() > deadline:
                raise TimeoutError('daemon startup')
            if self.endpoint.exists():
                try:
                    self.rpc('status')
                    break
                except (ConnectionRefusedError,FileNotFoundError):
                    pass
            time.sleep(.01)

    def stop(self, crash=False):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            try:
                code = self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait()
                raise
            self.proc = None
            if (crash and code != -signal.SIGKILL) or (not crash and code != 0):
                raise AssertionError(('unexpected daemon exit', code, crash))
        self.endpoint.unlink(missing_ok=True)

    def rpc(self, *fields, world=None, revision=None, error=False, sent=None, session=None):
        op = fields[0]
        if session is not None:
            fields += ("--session", session)
        if world is not None:
            fields += ('--world', world)
        if revision is not None:
            fields += ('--revision', revision)
        values = [v if isinstance(v, bytes) else str(v).encode() for v in fields]
        body = struct.pack('<I', len(values)) + b''.join(struct.pack('<I', len(v)) + v for v in values)
        event = {'op': op, 'phase': self.phase, 'world': world, 'revision': revision,
                 'args': [{'bytes': len(v), 'sha256': hashlib.sha256(v).hexdigest()} if isinstance(v, bytes)
                          else v for v in fields[1:]], 'start_ns': time.perf_counter_ns()}
        begin = event['start_ns']
        try:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(60)
                s.connect(str(self.endpoint))
                s.sendall(b'FFRPC001' + struct.pack('<I', len(body)) + body)
                if sent is not None:
                    sent.set()
                def exact(n):
                    result = b''
                    while len(result) < n:
                        part = s.recv(n - len(result))
                        if not part:
                            raise ConnectionError('truncated RPC response')
                        result += part
                    return result
                header = exact(12)
                if header[:8] != b'FFRSP001':
                    raise AssertionError('bad RPC response magic')
                size, = struct.unpack('<I', header[8:])
                if not 4 <= size <= 1024 * 1024:
                    raise AssertionError(('response length', size))
                response = exact(size)
                code, = struct.unpack('<I', response[:4])
                event['code'] = code
                if (code != 0) != error:
                    raise AssertionError((fields[:2], code, response[4:1024]))
                return response[4:]
        except Exception as exc:
            event['exception'] = repr(exc)
            raise
        finally:
            event['latency_ns'] = time.perf_counter_ns() - begin
            with self.lock:
                outcome = ('failed' if 'code' in event else 'interrupted') if 'exception' in event else ('rejected' if event['code'] else 'success')
                bucket = op + '/' + outcome
                self.outcomes[bucket] = self.outcomes.get(bucket, 0) + 1
                if outcome == 'success':
                    self.samples.setdefault(op, []).append(event['latency_ns'])
                    self.phase_samples.setdefault(self.phase + '/' + op, []).append(event['latency_ns'])
                self.coverage[op] = self.coverage.get(op, 0) + 1
                if op in {'fs-write', 'fs-save', 'fs-write-at', 'fs-append', 'fs-hwrite'}:
                    self.input_bytes += sum(len(v) for v in fields if isinstance(v, bytes))
                if op in MUTATIONS and outcome == 'success':
                    self.mutations += 1
                self.log.write(json.dumps(event) + '\n')

    def put(self, model, path, data, world='main', op='fs-write'):
        self.rpc(op, path, data, world=world)
        model[path] = Node('file', data, model[path].mode if path in model else 0o644)

    def verify(self, model, *, world=None, revision=None):
        selector = dict(world=world, revision=revision)
        for path, node in sorted(model.items()):
            info = json.loads(self.rpc('fs-lstat', path, **selector))
            assert info['directory'] == (node.kind == 'dir'), (path, info, node)
            assert info['symlink'] == (node.kind == 'symlink'), (path, info, node)
            assert info['mode'] == node.mode, (path, info, node)
            if node.kind == 'dir':
                expected = sorted(p.rsplit('/', 1)[-1] for p in model if p != '/' and (p.rsplit('/', 1)[0] or '/') == path)
                actual = self.rpc('fs-ls', path, **selector).decode().splitlines()
                assert sorted(actual) == expected, (path, actual, expected)
            else:
                op = 'fs-readlink' if node.kind == 'symlink' else 'fs-cat'
                assert self.rpc(op, path, **selector) == node.data, (path, world, revision)
                assert info['size'] == len(node.data), (path, info)

    def verify_all(self):
        assert set(self.rpc('fs-worlds').decode().splitlines()) == set(self.worlds) | self.probes
        assert set(self.rpc('fs-revisions').decode().splitlines()) == set(self.revisions)
        for name in sorted(self.probes):
            # Probe Worlds never change; rotate a source-file check across them.
            path = '/src/f' + str(int(name[5:]) % self.args.files) + '.c'
            assert self.rpc('fs-cat', path, world=name) == self.revisions['base'][path].data
        for name, model in list(self.worlds.items()):
            self.verify(model, world=name)
        for name, model in list(self.revisions.items()):
            self.verify(model, revision=name)
        self.rpc('fs-check')

    def measure_storage(self, stage):
        usage = disk_usage(self.db)
        unique = {hashlib.sha256(n.data).digest(): len(n.data)
                  for model in [*self.worlds.values(), *self.revisions.values()]
                  for n in model.values() if n.kind == 'file'}
        status = json.loads(self.rpc('status'))
        row = usage | {'stage': stage, 'live_main_bytes': sum(len(n.data) for n in self.worlds['main'].values()),
                       'unique_reachable_payload_bytes': sum(unique.values()),
                       'modeled_worlds': len(self.worlds), 'fork_probe_worlds': len(self.probes), 'retained_revisions': len(self.revisions),
                       'last_committed_seq': status['last_committed_seq'],
                       'indexed_objects': status['indexed_objects'], 'submitted_payload_bytes': self.input_bytes,
                       'mutation_requests': self.mutations}
        if self.storage:
            previous = self.storage[-1]
            commits = row['last_committed_seq'] - previous['last_committed_seq']
            growth = row['segment_bytes'] - previous['segment_bytes']
            row.update(delta_commits=commits, delta_segment_bytes=growth,
                       segment_bytes_per_commit=growth / commits if commits else None,
                       delta_allocated_bytes=row['allocated_bytes'] - previous['allocated_bytes'])
        row['segment_to_reachable_payload_ratio'] = row['segment_bytes'] / max(1, row['unique_reachable_payload_bytes'])
        self.storage.append(row)
        (self.root / 'storage.json').write_text(json.dumps(self.storage, indent=2))
        assert usage['allocated_bytes'] <= self.args.max_store_mib * 1024 * 1024, row
        return row

    def worker(self, round_no, index):
        rng = random.Random(self.args.seed + round_no * 100003 + index * 173)
        world = f'w{index}'
        if world not in self.worlds:
            self.rpc('fs-fork', 'base', world)
        else:
            self.rpc('fs-restore', 'base', world=world)
        model = copy.deepcopy(self.revisions['base'])
        self.worlds[world] = model
        for step in range(self.args.steps):
            path = f'/src/f{rng.randrange(self.args.files)}.c'
            choice = step % 7
            if choice == 0:  # Editor atomic save with variable-size content.
                data = f'int edit_{round_no}_{index}_{step} = {rng.randrange(100000)};\n'.encode()
                self.put(model, path, data, world, 'fs-save')
            elif choice == 1:  # Incremental overwrite, including sparse extension.
                offset = rng.randrange(len(model[path].data) + 5)
                data = rng.randbytes(rng.randrange(1, 33))
                self.rpc('fs-write-at', path, offset, data, world=world)
                old = model[path].data.ljust(offset, b'\0')
                model[path].data = old[:offset] + data + old[offset + len(data):]
            elif choice == 2:
                data = f'// build {round_no}-{index}-{step}\n'.encode()
                answer = json.loads(self.rpc('fs-append', path, data, world=world))
                assert answer == {'offset': len(model[path].data), 'written': len(data)}
                model[path].data += data
            elif choice == 3:
                size = rng.randrange(0, 128)
                self.rpc('fs-truncate', path, size, world=world)
                model[path].data = model[path].data[:size].ljust(size, b'\0')
            elif choice == 4:  # Rename over an existing tracked file, then recreate it.
                target = f'/src/f{(int(Path(path).stem[1:]) + 1) % self.args.files}.c'
                self.rpc('fs-rename', path, target, world=world)
                model[target] = model.pop(path)
                self.put(model, path, b'int recreated = 1;\n', world)
            elif choice == 5:
                self.rpc('fs-chmod', path, '755', world=world);model[path].mode = 0o755
            else:
                self.rpc('fs-rename', '/src/missing', path, world=world, error=True)
                self.rpc('fs-write', path, b'forbidden', revision='base', error=True)
            assert self.rpc('fs-cat', path, world=world) == model[path].data
        # Dependency hashing and rebuild/clean churn. Artifacts are synthetic,
        # deterministic and large enough to cross the 64 KiB chunk boundary.
        digest = hashlib.sha256(b''.join(model[p].data for p in sorted(model) if p.startswith('/src/'))).digest()
        artifact = digest * 2305
        for cycle in range(2):
            self.put(model, '/build/module.o', artifact, world)
            assert self.rpc('fs-cat', '/build/module.o', world=world) == artifact
            self.rpc('fs-unlink', '/build/module.o', world=world);del model['/build/module.o']
        # Lock/temp lifetime, mkdir/rmdir, link preservation, symlink resolution.
        self.put(model, '/build/tool.lock', b'locked', world)
        self.rpc('fs-unlink', '/build/tool.lock', world=world);del model['/build/tool.lock']
        self.rpc('fs-mkdir', '/build/tmp', world=world)
        self.rpc('fs-rmdir', '/build/tmp', world=world)
        self.rpc('fs-link', '/src/f0.c', '/build/alias', world=world)
        source = json.loads(self.rpc('fs-stat', '/src/f0.c', world=world))
        alias = json.loads(self.rpc('fs-stat', '/build/alias', world=world))
        assert source['inode_id'] == alias['inode_id'] and source['links'] == alias['links'] == 2
        self.rpc('fs-unlink', '/build/alias', world=world)
        assert json.loads(self.rpc('fs-stat', '/src/f0.c', world=world))['links'] == 1
        # Tools keep descriptors open across unlink; locks belong to open descriptions.
        session = json.loads(self.rpc('session-open', '60000'))['session']
        handle_args = dict(world=world, session=session)
        self.put(model, '/build/open.tmp', b'original', world)
        first = json.loads(self.rpc('fs-open', '/build/open.tmp', 'rw', **handle_args))['handle']
        second = json.loads(self.rpc('fs-open', '/build/open.tmp', 'rw', **handle_args))['handle']
        assert json.loads(self.rpc('fs-hlock', first, 'exclusive', **handle_args))['granted']
        assert not json.loads(self.rpc('fs-hlock', second, 'exclusive', **handle_args))['granted']
        self.rpc('fs-unlink', '/build/open.tmp', world=world);del model['/build/open.tmp']
        assert json.loads(self.rpc('fs-hstat', first, **handle_args))['links'] == 0
        self.rpc('fs-hwrite', first, '0', b'changed!', **handle_args)
        assert self.rpc('fs-hread', second, '0', '8', **handle_args) == b'changed!'
        self.rpc('fs-close', first, **handle_args)
        assert json.loads(self.rpc('fs-hlock', second, 'exclusive', **handle_args))['granted']
        self.rpc('fs-close', second, **handle_args)
        self.rpc('session-close', session)
        self.rpc('fs-symlink', '../src/f0.c' , '/build/current', world=world)
        assert self.rpc('fs-readlink', '/build/current', world=world) == b'../src/f0.c'
        assert self.rpc('fs-cat', '/build/current', world=world) == model['/src/f0.c'].data
        self.rpc('fs-unlink', '/build/current', world=world)
        name = f'r{round_no}w{index}'
        self.rpc('fs-snapshot', name, world=world)
        with self.lock:
            self.revisions[name] = copy.deepcopy(model)
        self.verify(model, world=world)

    def crash_inflight(self, round_no):
        old = self.worlds['main']['/config/settings'].data
        new = random.Random(self.args.seed + round_no).randbytes(128 * 1024)
        sent = threading.Event()
        outcome = {}
        def writer():
            try:
                self.rpc('fs-save', '/config/settings', new, world='main', sent=sent)
                outcome['acked'] = True
            except (OSError, ConnectionError):
                outcome['acked'] = False
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            task = pool.submit(writer)
            assert sent.wait(10), 'writer did not send request'
            # Vary the crash point without requiring a particular scheduling outcome.
            time.sleep((round_no % 3) * .001)
            self.stop(crash=True)
            task.result(timeout=10)
        self.start()
        actual = self.rpc('fs-cat', '/config/settings', world='main')
        check_recovered_save(old, new, actual, outcome['acked'])
        self.worlds['main']['/config/settings'] = Node('file', actual)
        self.crash_trials += 1
        self.log.write(json.dumps({'crash_trial': round_no, 'acked': outcome['acked'],
                                   'recovered': 'new' if actual == new else 'old'}) + '\n')

    def run(self):
        self.cli('init', self.db);self.start();self.rpc('fs-init')
        self.phase = 'checkout'
        model = self.worlds['main']
        for name in ('src', 'include', 'config', 'build', '.cache'):
            self.rpc('fs-mkdir', '/' + name);model['/' + name] = Node('dir', mode=0o755)
        rng = random.Random(self.args.seed)
        for i in range(self.args.files):
            # Unique source payloads prevent a dedup-only checkout fixture.
            header = f'int function_{i}(void) {{ return {rng.randrange(1000000)}; }}\n'.encode()
            size = self.args.file_bytes * rng.choice((1, 1, 1, 4))
            payload = header + b'/*' + b'x' * (size - len(header) - 4) + b'*/'
            self.put(model, f'/src/f{i}.c', payload)
        self.put(model, '/include/api.h', b'#pragma once\nint entry(void);\n')
        self.put(model, '/config/settings', b'{"debug":true}\n')
        self.rpc('fs-snapshot', 'base');self.revisions['base'] = copy.deepcopy(model)
        self.measure_storage('checkout')
        self.phase = 'fork-probe'
        before = self.measure_storage('fork-probe-before')
        for i in range(16):
            self.rpc('fs-fork', 'base', f'probe{i}')
            self.probes.add(f'probe{i}')
        # The same manifest is shared; forking must not append a copy of the tree.
        after = self.measure_storage('fork-probe-after')
        assert after['segment_bytes'] - before['segment_bytes'] <= 16 * 8192, (before, after)
        self.verify(self.revisions['base'], world='probe15')
        for round_no in range(self.args.rounds):
            self.phase = 'development'
            before = self.measure_storage(f'round-{round_no}-before')
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.args.workers) as pool:
                list(pool.map(lambda i: self.worker(round_no, i), range(self.args.workers)))
            self.phase = 'verification'
            self.verify_all()
            after = self.measure_storage(f'round-{round_no}-after')
            # Detect gross copying while allowing metadata history and no GC.
            budget = (self.input_bytes - before['submitted_payload_bytes']
                      + (self.mutations - before['mutation_requests']) * 256 * 1024 + 1024 * 1024)
            assert after['segment_bytes'] - before['segment_bytes'] <= budget, (before, after, budget)
            if (round_no + 1) % self.args.restart_every == 0:
                self.phase = 'recovery'
                self.stop(crash=True);self.start();self.barrier_crashes += 1
                self.verify_all()  # Every previous ACK must survive.
                self.crash_inflight(round_no);self.verify_all()
            self.measure_storage(f'round-{round_no}-recovered')
            self.report('running')
            print(f'round {round_no + 1}/{self.args.rounds}: verified; allocated={after["allocated_bytes"]}', flush=True)
        # Read-only traffic must neither advance sequence nor append objects.
        self.phase = 'read-only'
        before = self.measure_storage('read-only-before')
        for _ in range(3):
            self.verify(self.revisions['base'], revision='base')
        after = self.measure_storage('read-only-after')
        assert after['last_committed_seq'] == before['last_committed_seq']
        assert after['segment_bytes'] == before['segment_bytes']
        self.stop()
        self.phase = 'backup'
        self.cli('backup', self.db, self.root / 'backup.pack')
        restored = self.root / 'restored'
        self.cli('replay', self.root / 'backup.pack', restored)
        self.phase = 'restored-verification'
        self.start(restored);self.verify_all();self.stop()
        self.measure_offline_restored = disk_usage(restored)
        self.backup_bytes = (self.root / 'backup.pack').stat().st_size
        assert self.measure_offline_restored['allocated_bytes'] <= self.args.max_store_mib * 1024 * 1024
        self.report('passed')

    def report(self, state, failure=None):
        result = {'elapsed_seconds': time.monotonic() - self.started, 'state': state, 'seed': self.args.seed, 'coverage': self.coverage, 'outcomes': self.outcomes,
                  'latency_scope': 'successful RPCs only; rejects and interruptions counted separately',
                  'latency': {k: distribution(v) for k, v in self.samples.items()},
                  'storage': self.storage, 'process_crash_trials': self.crash_trials + self.barrier_crashes,
                  'inflight_save_crash_trials': self.crash_trials, 'acked_barrier_restarts': self.barrier_crashes,
                  'latency_by_phase': {k: distribution(v) for k, v in self.phase_samples.items()},
                  'restored_storage': getattr(self, 'measure_offline_restored', None),
                  'backup_bytes': getattr(self, 'backup_bytes', None),
                  'failure': failure,
                  'limits': ['RPC workload, not mounted git/compiler performance',
                             'process crashes, not physical power loss',
                             'historical objects retained; no reclamation claim',
                             'allocated bytes use st_blocks; APFS sharing/compression may differ']}
        (self.root / 'summary.json').write_text(json.dumps(result, indent=2))

    def close(self):
        try:
            if self.proc is not None:
                self.stop()
        finally:
            self.log.close();self.daemon_log.close()
            self.endpoint.unlink(missing_ok=True)
            self.runtime.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=173)
    parser.add_argument('--files', type=int, default=32)
    parser.add_argument('--file-bytes', type=int, default=1024)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--steps', type=int, default=7)
    parser.add_argument('--restart-every', type=int, default=1)
    parser.add_argument('--max-store-mib', type=int, default=512)
    args = parser.parse_args()
    if not __debug__:
        parser.error('run without -O or PYTHONOPTIMIZE; the correctness oracle requires assertions')
    if not 64 <= args.file_bytes <= 65536:
        parser.error('file-bytes must be 64..65536 (some fixtures are four times this size)')
    if args.files < 2 or min(args.rounds, args.workers, args.steps, args.restart_every, args.max_store_mib) < 1 or args.workers > 8:
        parser.error('files >= 2, workers <= 8; all counts and budgets must be positive')
    trial = Workload(args)
    try:
        trial.run()
    except BaseException:
        trial.report('failed', traceback.format_exc())
        raise
    finally:
        trial.close()


if __name__ == '__main__':
    main()
