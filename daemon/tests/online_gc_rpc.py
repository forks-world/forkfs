"""Manager RPC controls for background online collection."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

BIN = str(Path(sys.argv[1]).resolve())

def cli(*args, ok=True):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=20)
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix='ff-online-gc-rpc-') as directory:
    root = Path(directory)
    db, endpoint, payload = root/'db', root/'rpc.sock', root/'input'
    cli('init', db)

    def request(*args, ok=True):
        return cli('request', endpoint, *args, ok=ok)

    def obj(*args, ok=True):
        return json.loads(request(*args, ok=ok))

    def start_server():
        proc = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 10
        last_probe = None
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'daemon exited before RPC readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                try:
                    probe = subprocess.run([BIN, 'request', str(endpoint), 'gc-status'],
                                           capture_output=True, timeout=2)
                    if probe.returncode == 0:
                        status = json.loads(probe.stdout)
                        assert 'state' in status and 'active' in status, status
                        if proc.poll() is not None:
                            stdout, stderr = proc.communicate()
                            raise AssertionError(f'daemon exited after binding: {stdout!r} {stderr!r}')
                        return proc
                    last_probe = (probe.returncode, probe.stdout, probe.stderr)
                except subprocess.TimeoutExpired as error:
                    last_probe = error
            if time.monotonic() >= deadline:
                proc.terminate()
                try:
                    stdout, stderr = proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); stdout, stderr = proc.communicate()
                raise AssertionError(f'daemon RPC readiness timed out; probe={last_probe!r}; logs={stdout!r} {stderr!r}')
            time.sleep(.01)

    def finish_gc(timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            state = obj('gc-status')
            assert set(('state', 'active', 'steps', 'last_step', 'error')) <= state.keys(), state
            if not state['active']:
                assert state['state'] in {'complete', 'blocked', 'budget_exhausted', 'stopped', 'failed'}, state
                assert state['error'] == '', state
                return state
            assert time.monotonic() < deadline, state
            time.sleep(.01)

    proc = start_server()
    try:
        request('fs-init')
        assert json.loads(request('fs-sync'))['synced'] is True
        before = obj('status')['last_committed_seq']
        file_value = bytes((i * 31 + 7) % 251 for i in range(180000))
        payload.write_bytes(file_value)
        for _ in range(5):
            request('fs-write', '/file', payload)
        held = b'open unlink handle survives concurrent collection'
        payload.write_bytes(held)
        request('fs-write', '/held', payload)
        session = obj('session-open', '60000')['session']
        handle = json.loads(request('fs-open', '/held', 'rw', '--session', session))['handle']
        request('fs-unlink', '/held')

        # Reject invalid bounds before creating a worker or changing status.
        for args in (('gc-start', '0', '1', '1'), ('gc-start', '-1', '1', '1'),
                     ('gc-start', '65580', '0', '1'), ('gc-start', '8388609', '1', '1'),
                     ('gc-start', '65580', '4097', '1'), ('gc-start', '65580', '1', '1025'),
                     ('gc-start', '65580', '1'), ('gc-step', '0'),
                     ('gc-start', '65580', '1', '1', '65535'),
                     ('gc-start', '65580', '1', '1', '268435457'),
                     ('gc-step', '65580', '1', '1048576', '0'),
                     ('gc-step', '65580', '1', '1048576', '65537'),
                     ('gc-step', '65580', '1', '65535'),
                     ('gc-step', '65580', '1', '268435457')):
            request(*args, ok=False)
        # Session selectors cannot turn manager-only GC controls into session operations.
        request('gc-status', '--session', session, ok=False)
        request('gc-start', '--session', session, ok=False)

        one_unit = obj('gc-step', '65580', '1', '1048576', '1')
        assert one_unit['mark_work_done'] <= 1, one_unit
        assert one_unit['mark_pending'] and one_unit['mark_phase'] != 'complete', one_unit

        metadata_budget = 1048576
        started = obj('gc-start', '65580', '4', '24', str(metadata_budget))
        assert started['state'] in {'running', 'complete', 'blocked', 'budget_exhausted'}, started
        assert started['error'] == '', started

        # Keep the manager connection useful during background work and mutate
        # the open, unlinked file through its session-owned handle.
        final_value = file_value
        for index in range(4):
            final_value = bytes((i * (37 + index) + 13 + index) % 253 for i in range(165000 + index * 4096))
            payload.write_bytes(final_value)
            request('fs-write', '/file', payload)
            assert request('fs-cat', '/file') == final_value
            held_next = held + bytes([65 + index])
            payload.write_bytes(bytes([65 + index]))
            request('fs-hwrite', handle, str(len(held)), payload, '--session', session)
            held = held_next
            assert request('fs-hread', handle, '0', str(len(held)), '--session', session) == held
            obj('gc-status')

        completed = finish_gc()
        assert completed['steps'] > 0, completed
        assert completed['last_step']['metadata_budget_bytes'] == metadata_budget, completed
        assert completed['last_step']['metadata_bytes_charged'] <= metadata_budget, completed
        after = obj('status')['last_committed_seq']
        assert after > before, (before, after)
        assert request('fs-cat', '/file') == final_value
        assert request('fs-check')
        request('fs-close', handle, '--session', session)
        request('session-close', session)

        # A second job can start after the first is idle; stop joins it and
        # returns a terminal, inspectable status whether it stopped or finished.
        again = obj('gc-start', '65580', '1', '1024')
        assert again['state'] in {'running', 'complete', 'blocked', 'budget_exhausted'}, again
        stopped = obj('gc-stop')
        assert not list(db.glob('.gc-mark-*')), 'gc-stop retained scratch state'
        assert not stopped['active'] and stopped['state'] in {
            'stopped', 'complete', 'blocked', 'budget_exhausted'
        }, stopped
        assert stopped['error'] == '', stopped
        restarted = obj('gc-start', '65580', '2', '1')
        assert restarted['state'] in {'running', 'complete', 'blocked', 'budget_exhausted'}, restarted
        terminal = finish_gc()
        assert terminal['steps'] > 0 and terminal['error'] == '', terminal
        request('fs-check')
        assert request('fs-cat', '/file') == final_value
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait(); raise

print('manager RPC online GC start, status, stop, validation and live I/O passed')
