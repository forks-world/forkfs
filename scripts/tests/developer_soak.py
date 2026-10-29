"""Bounded seeded RPC soak with online GC, storage sampling and online backup.

This exercises the resident Unix RPC development workload. It does not run a
mounted checkout, git command, compiler, or other production application.
"""
import argparse
import concurrent.futures
import json
import hashlib
import math
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback

import developer_workload as _workload


def rpc_distribution(samples):
    return _workload.distribution(samples)


def gc_job_rates(events):
    """Completed logical/physical steps per observed job wall time, not MB/s."""
    jobs = {}
    for event in events:
        attempt = event.get('gc_attempt')
        if attempt is None:
            continue
        if event['kind'] == 'gc-start':
            jobs[attempt] = {'attempt': attempt, 'requested_perf_ns': event['requested_perf_ns']}
        job = jobs.get(attempt)
        state = event.get('status', {})
        if job and state.get('active') is False and 'state' not in job:
            seconds = (event['perf_ns'] - job['requested_perf_ns']) / 1e9
            if seconds > 0:
                job.update(state=state['state'], steps=state['steps'], observed_seconds=seconds,
                           completed_steps_per_second=state['steps'] / seconds)
    return list(jobs.values())


class Soak(_workload.Workload):
    def __init__(self, args):
        super().__init__(args)
        self.deadline_at = time.monotonic() + args.deadline
        self.sample_stop = threading.Event()
        self.sample_thread = None
        self.sample_count = 0
        self.storage_peak_lock = threading.Lock()
        self.storage_peaks = {key: 0 for key in ('allocated_bytes', 'segment_bytes', 'index_bytes',
                                                  'control_bytes', 'mark_scratch_bytes',
                                                  'physical_scratch_bytes', 'scratch_allocated_bytes',
                                                  'artifact_allocated_bytes', 'artifact_apparent_bytes')}
        self.storage_samples_log = (self.root / 'storage-samples.jsonl').open('w', buffering=1)
        self.background_errors = []
        self.job_events = []
        self.job_log = (self.root / 'jobs.jsonl').open('w', buffering=1)
        self.job_lock = threading.Lock()
        self.gc_round = 0
        self.rpc_requests = 0
        self.logical_sweep_skipped = False
        self.backup_timeline = {}
        self.rpc_intervals = []
        self.rpc_interval_lock = threading.Lock()
        self.backup_monitor_stop = threading.Event()
        self.backup_monitor_thread = None
        self.backup_running_observations = []
        self.gc_enabled = args.gc_mode == 'concurrent'
        self.gc_supervisor_stop = threading.Event()
        self.gc_supervisor_thread = None
        self.gc_active_observations = []
        self.gc_foreground_overlap = []
        self.gc_observation_lock = threading.Lock()
        self.shutdown_phase = False
        self.pre_backup_gc_storage = None
        self.replayed_storage = {}
        self.retained_revision_counts = []
        snapshots = self.root / 'harness-snapshot'
        snapshots.mkdir()
        self.settings['harness_sha256'] = {}
        for name in (__file__, _workload.__file__):
            source = Path(name).read_bytes()
            self.settings['harness_sha256'][Path(name).name] = hashlib.sha256(source).hexdigest()
            (snapshots / Path(name).name).write_bytes(source)
        self.settings['gc_mode'] = args.gc_mode
        self.settings['worker_revision_pattern'] = 'r{generation}w{worker}'
        self.settings['revision_policy'] = 'retain all; no revision deletion API is used'
        (self.root / 'settings.json').write_text(json.dumps(self.settings, default=str, indent=2))

    def remaining(self):
        if self.shutdown_phase:
            return 5.0
        left = self.deadline_at - time.monotonic()
        if left <= 0:
            raise TimeoutError('overall soak deadline exceeded')
        return left

    def rpc_timeout(self):
        return min(60.0, self.remaining())

    def rpc(self, *fields, **kwargs):
        if not self.shutdown_phase:
            self.check_background()
            with self.lock:
                if self.rpc_requests >= self.args.max_rpcs:
                    raise AssertionError('RPC recording budget exceeded')
                self.rpc_requests += 1
        result = super().rpc(*fields, **kwargs)
        if not self.shutdown_phase:
            self.check_background()
        return result

    def rpc_observed(self, event):
        # Use the actual transport timer, excluding trace I/O and model work.
        with self.rpc_interval_lock:
            self.rpc_intervals.append({'thread': event['thread'], 'start_ns': event['start_ns'],
                                       'end_ns': event['start_ns'] + event['latency_ns'],
                                       'op': event['op']})

    def cli(self, *args):
        timeout = min(120, self.remaining())
        result = subprocess.run([self.binary, *map(str, args)], capture_output=True, timeout=timeout)
        self.remaining()
        if result.returncode:
            raise AssertionError((args, result.returncode, result.stderr.decode(errors='replace')))
        return result.stdout

    def event(self, kind, **fields):
        row = {'time_ns': time.time_ns(), 'perf_ns': time.perf_counter_ns(), 'elapsed_seconds': time.monotonic() - self.started,
               'phase': self.phase, 'gc_mode': self.args.gc_mode, 'kind': kind} | fields
        if kind.startswith('gc-'):
            row['gc_attempt'] = self.gc_round
        with self.job_lock:
            self.job_events.append(row)
            self.job_log.write(json.dumps(row) + '\n')

    def start_sampler(self):
        def sample():
            try:
                while not self.sample_stop.is_set():
                    if self.db.exists():
                        row = _workload.disk_usage(self.db)
                        artifacts = _workload.disk_usage(self.root)
                        row['artifact_allocated_bytes'] = artifacts['allocated_bytes']
                        row['artifact_apparent_bytes'] = artifacts['apparent_bytes']
                        row.update(time_ns=time.time_ns(), elapsed_seconds=time.monotonic() - self.started,
                                   phase=self.phase)
                        self.storage_samples_log.write(json.dumps(row) + '\n')
                        with self.storage_peak_lock:
                            self.sample_count += 1
                            for key in self.storage_peaks:
                                self.storage_peaks[key] = max(self.storage_peaks[key], row.get(key, 0))
                        if row['allocated_bytes'] > self.args.max_store_mib * 1024 * 1024:
                            raise AssertionError(('storage budget exceeded', row))
                        if row['artifact_allocated_bytes'] > self.args.max_artifact_mib * 1024 * 1024:
                            raise AssertionError(('artifact storage budget exceeded', row))
                    self.sample_stop.wait(self.args.sample_interval)
            except BaseException:
                self.background_errors.append(traceback.format_exc())
                self.sample_stop.set()
        self.sample_thread = threading.Thread(target=sample, name='storage-sampler', daemon=True)
        self.sample_thread.start()

    def check_background(self):
        self.remaining()
        if self.background_errors:
            raise AssertionError('background task failed:\n' + '\n'.join(self.background_errors))

    def gc_status(self):
        return json.loads(self.rpc('gc-status'))

    def start_gc(self, stage):
        self.gc_round += 1
        requested_perf_ns = time.perf_counter_ns()
        result = json.loads(self.rpc('gc-start', self.args.gc_copy_bytes,
                                     self.args.gc_objects, 1024,
                                     64 * 1024 * 1024, self.args.gc_work))
        self.event('gc-start', stage=stage, attempt=self.gc_round, requested_perf_ns=requested_perf_ns, status=result)
        return result

    def start_gc_supervisor(self, stage):
        self.gc_supervisor_stop.clear()

        def supervise():
            try:
                while not self.gc_supervisor_stop.is_set():
                    self.remaining()
                    status = self.gc_status()
                    observed = time.perf_counter_ns()
                    self.event('gc-supervisor-status', stage=stage, status=status,
                               observed_perf_ns=observed)
                    if status['active']:
                        with self.gc_observation_lock:
                            self.gc_active_observations.append({'phase': stage, 'observed_perf_ns': observed})
                    elif status['state'] in {'complete', 'budget_exhausted', 'blocked'}:
                        self.start_gc(stage + '-resume')
                    else:
                        raise AssertionError(('unexpected inactive GC supervisor state', status))
                    self.gc_supervisor_stop.wait(.1)
            except BaseException:
                self.background_errors.append(traceback.format_exc())
                self.gc_supervisor_stop.set()

        self.gc_supervisor_thread = threading.Thread(target=supervise, name='gc-supervisor', daemon=True)
        self.gc_supervisor_thread.start()

    def stop_gc_supervisor(self):
        self.gc_supervisor_stop.set()
        if self.gc_supervisor_thread:
            self.gc_supervisor_thread.join(timeout=5)
            if self.gc_supervisor_thread.is_alive():
                raise TimeoutError('GC supervisor did not stop')
            self.gc_supervisor_thread = None

    def wait_gc_terminal(self, stage):
        while True:
            self.check_background()
            status = self.gc_status()
            self.event('gc-status', stage=stage, status=status)
            if not status['active']:
                if status['state'] == 'failed':
                    raise AssertionError(('GC failed', status))
                return status
            time.sleep(min(.2, self.remaining()))

    def gc_report(self, status):
        last = status.get('last_step')
        if isinstance(last, str):
            try:
                report = json.loads(last) if last else {}
            except ValueError as error:
                raise AssertionError(('malformed GC last_step JSON', last)) from error
        elif isinstance(last, dict):
            report = last
        elif last is None:
            report = {}
        else:
            raise AssertionError(('unexpected GC last_step type', type(last).__name__, last))
        if not isinstance(report, dict):
            raise AssertionError(('GC last_step is not an object', report))
        return report

    def drain_gc(self, stage, allow_logical_skip=False):
        while True:
            status = self.gc_status()
            if status['active']:
                status = self.wait_gc_terminal(stage)
            if status['state'] not in {'complete', 'budget_exhausted', 'blocked'}:
                raise AssertionError(('GC did not drain', status))
            report = self.gc_report(status)
            skipped = bool(report.get('logical_sweep_skipped'))
            self.logical_sweep_skipped |= skipped
            physically_drained = (report.get('physical_pending') is False
                                  and report.get('mark_pending') is False
                                  and report.get('pending_segments') == 0)
            if skipped and allow_logical_skip and physically_drained:
                return status
            if status['state'] == 'complete':
                if not physically_drained:
                    raise AssertionError(('GC complete with pending physical/mark work', status))
                if allow_logical_skip and not skipped:
                    self.start_gc(stage)
                    continue
                return status
            if status['state'] not in {'budget_exhausted', 'blocked'}:
                raise AssertionError(('GC did not drain', status))
            # Start a new bounded worker directly; do not issue gc-stop between budgets.
            self.start_gc(stage)

    def _checkout(self):
        self.cli('init', self.db)
        self.start()
        self.rpc('fs-init')
        self.phase = 'checkout'
        model = self.worlds['main']
        for name in ('src', 'include', 'config', 'build', '.cache'):
            self.rpc('fs-mkdir', '/' + name)
            model['/' + name] = _workload.Node('dir', mode=0o755)
        import random
        rng = random.Random(self.args.seed)
        for i in range(self.args.files):
            header = f'int function_{i}(void) {{ return {rng.randrange(1000000)}; }}\n'.encode()
            size = self.args.file_bytes * rng.choice((1, 1, 1, 4))
            self.put(model, f'/src/f{i}.c', header + b'/*' + b'x' * (size - len(header) - 4) + b'*/')
        self.put(model, '/include/api.h', b'#pragma once\nint entry(void);\n')
        self.put(model, '/config/settings', b'{"debug":true}\n')
        self.rpc('fs-snapshot', 'base')
        self.revisions['base'] = __import__('copy').deepcopy(model)
        self.measure_storage('checkout')

    def run_round(self, round_no, label):
        # Only this round's intervals are needed; raw operations stay on disk.
        with self.rpc_interval_lock:
            self.rpc_intervals.clear()
        if self.gc_enabled:
            current_gc = self.gc_status()
        else:
            current_gc = None
        if self.gc_enabled and not current_gc['active'] and label == 'development':
            # Keep an online collector overlapping producer rounds when an earlier
            # bounded collection completed or exhausted its step budget.
            if current_gc['state'] not in {'complete', 'budget_exhausted', 'blocked'}:
                raise AssertionError(('unexpected inactive GC state during development', current_gc))
            self.start_gc('development-resume')
        phase_label = f'{label}/{self.args.gc_mode}'
        self.phase = phase_label
        round_start_wall = time.time_ns()
        round_start_perf = time.perf_counter_ns()
        before = self.measure_storage(label + '-before')
        if self.gc_enabled:
            self.start_gc_supervisor(label)
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=self.args.workers,
                                                       thread_name_prefix='soak-worker') as pool:
                futures = [pool.submit(self.worker, round_no, i) for i in range(self.args.workers)]
                for future in futures:
                    future.result(timeout=self.remaining())
        finally:
            if self.gc_enabled:
                self.stop_gc_supervisor()
        self.check_background()
        self.phase = phase_label + '-verify'
        self.verify_all()
        after = self.measure_storage(label + '-after')
        # Retained revisions deliberately make flat-space claims invalid. This only guards runaway growth.
        if after['allocated_bytes'] > self.args.max_store_mib * 1024 * 1024:
            raise AssertionError(('storage budget exceeded', after))
        self.event('round-complete', round=round_no, stage=label,
                   generation=round_no, retained_revisions=len(self.revisions),
                   commits=after['last_committed_seq'], allocated_bytes=after['allocated_bytes'])
        with self.rpc_interval_lock:
            worker_rpcs = [row for row in self.rpc_intervals if row['thread'].startswith('soak-worker')
                           and row['start_ns'] < time.perf_counter_ns()
                           and row['end_ns'] > round_start_perf]
        active_observed = [item for item in self.gc_active_observations
                           if item['phase'] == label
                           and any(row['start_ns'] <= item['observed_perf_ns'] <= row['end_ns']
                                   for row in worker_rpcs)]
        self.gc_foreground_overlap.append({'phase': label, 'gc_mode': self.args.gc_mode,
                                           'worker_rpc_count': len(worker_rpcs),
                                           'gc_active_observations': len(active_observed),
                                           'overlap_observed': bool(active_observed)})
        self.retained_revision_counts.append({'round': round_no, 'generation': round_no,
                                              'retained_revisions': len(self.revisions)})
        return {'start_wall_ns': round_start_wall, 'wall_ns': time.time_ns(), 'perf_ns': time.perf_counter_ns(),
                'start_perf_ns': round_start_perf}

    def start_backup(self, request_id, archive):
        self.backup_timeline[request_id] = {'requested_ns': time.time_ns(),
                                            'requested_perf_ns': time.perf_counter_ns(),
                                            'archive': str(archive)}
        status = json.loads(self.rpc('backup-start', request_id, archive))
        self.backup_timeline[request_id]['start_status'] = status
        self.backup_timeline[request_id]['start_returned_ns'] = time.time_ns()
        self.backup_timeline[request_id]['start_returned_perf_ns'] = time.perf_counter_ns()
        self.event('backup-start', request_id=request_id, status=status)

    def start_backup_monitor(self, request_id):
        self.backup_monitor_stop.clear()
        self.backup_running_observations = []

        def monitor():
            try:
                while not self.backup_monitor_stop.is_set():
                    self.remaining()
                    status = json.loads(self.rpc('backup-status', request_id))
                    observed_ns = time.perf_counter_ns()
                    self.event('backup-monitor-status', request_id=request_id, status=status,
                               observed_perf_ns=observed_ns)
                    if status['state'] == 'running':
                        self.backup_running_observations.append(observed_ns)
                    elif status['state'] != 'durable':
                        raise AssertionError(('backup monitor saw non-durable terminal state', status))
                    else:
                        break
                    self.backup_monitor_stop.wait(.01)
            except BaseException:
                self.background_errors.append(traceback.format_exc())
                self.backup_monitor_stop.set()

        self.backup_monitor_thread = threading.Thread(target=monitor, name='backup-monitor', daemon=True)
        self.backup_monitor_thread.start()

    def stop_backup_monitor(self):
        self.backup_monitor_stop.set()
        if self.backup_monitor_thread:
            self.backup_monitor_thread.join(timeout=max(1, min(5, self.remaining())))
            if self.backup_monitor_thread.is_alive():
                raise TimeoutError('backup status monitor did not stop')
            self.backup_monitor_thread = None

    def await_backup(self, request_id):
        while True:
            self.check_background()
            status = json.loads(self.rpc('backup-status', request_id))
            self.event('backup-status', request_id=request_id, status=status)
            if status['state'] != 'running':
                if status['state'] != 'durable':
                    raise AssertionError(('backup not durable', status))
                self.backup_timeline[request_id]['durable_ns'] = time.time_ns()
                self.backup_timeline[request_id]['durable_perf_ns'] = time.perf_counter_ns()
                self.backup_timeline[request_id]['durable_status'] = status
                return status
            time.sleep(min(.1, self.remaining()))

    def check_artifact_budget(self):
        usage = _workload.disk_usage(self.root)
        if usage['allocated_bytes'] > self.args.max_artifact_mib * 1024 * 1024:
            raise AssertionError(('artifact storage budget exceeded', usage))
        return usage

    def replay_verify(self, archive, directory, base_revision_only=False):
        self.cli('replay', archive, directory)
        usage = _workload.disk_usage(directory)
        self.replayed_storage[Path(directory).name] = usage
        if usage['allocated_bytes'] > self.args.max_store_mib * 1024 * 1024:
            raise AssertionError(('replayed repository storage budget exceeded', usage))
        self.check_artifact_budget()
        source_proc, source_endpoint = self.proc, self.endpoint
        self.endpoint = self.runtime / 'replay.sock'
        try:
            self.start(directory)
            if base_revision_only:
                self.verify(self.revisions['base'], revision='base')
                self.rpc('fs-check')
            else:
                self.verify_all()
        finally:
            self.stop()
            self.proc, self.endpoint = source_proc, source_endpoint

    def run(self):
        self.start_sampler()
        self._checkout()
        self.phase = 'development'
        stop_at = min(self.deadline_at, time.monotonic() + self.args.seconds) if self.args.seconds else self.deadline_at
        if self.gc_enabled:
            self.start_gc('development')
        round_no = 0
        while round_no < self.args.rounds and time.monotonic() < stop_at:
            self.run_round(round_no, 'development')
            round_no += 1
            self.check_background()
        if self.gc_enabled:
            # Record a storage checkpoint around the final logical + physical drain.
            before_gc_drain = self.measure_storage('pre-backup-gc-before')
            drain_started = time.perf_counter()
            state = self.gc_status()
            if not state['active'] and state['state'] in {'complete', 'budget_exhausted', 'blocked'}:
                self.start_gc('pre-backup-final-drain')
            while True:
                state = self.wait_gc_terminal('pre-backup')
                if state['state'] == 'complete':
                    report = self.gc_report(state)
                    if report.get('logical_sweep_skipped') or report.get('physical_pending') is not False \
                            or report.get('mark_pending') is not False or report.get('pending_segments') != 0:
                        raise AssertionError(('pre-backup GC did not fully drain logical and physical work',
                                              state, report))
                    break
                if state['state'] not in {'budget_exhausted', 'blocked'}:
                    raise AssertionError(('pre-backup GC failed to finish', state))
                self.start_gc('pre-backup-resume')
            drain_seconds = time.perf_counter() - drain_started
            after_gc_drain = self.measure_storage('pre-backup-gc-after')
            self.pre_backup_gc_storage = {
                'before': before_gc_drain, 'after': after_gc_drain,
                'drain_seconds': drain_seconds,
                'observed_net_segment_reclaimed_bytes_per_second': (before_gc_drain['segment_bytes'] - after_gc_drain['segment_bytes']) / drain_seconds,
                'observed_allocated_delta_bytes': after_gc_drain['allocated_bytes'] - before_gc_drain['allocated_bytes'],
                'observed_segment_delta_bytes': after_gc_drain['segment_bytes'] - before_gc_drain['segment_bytes'],
                'observed_allocated_reduction_bytes': before_gc_drain['allocated_bytes'] - after_gc_drain['allocated_bytes'],
                'observed_segment_reduction_bytes': before_gc_drain['segment_bytes'] - after_gc_drain['segment_bytes'],
                'interpretation': 'observed sample delta; storage is not expected to shrink monotonically',
            }
        self.phase = 'pre-backup-verify'
        self.verify_all()
        base_archive = self.root / 'base.pack'
        self.phase = 'online-base-backup'
        self.start_backup('base', base_archive)
        if self.gc_enabled:
            self.start_gc('backup-overlap')
        self.start_backup_monitor('base')
        round_timing = self.run_round(round_no, 'backup-overlap')
        round_no += 1
        durable = self.await_backup('base')
        self.stop_backup_monitor()
        self.check_background()
        timeline = self.backup_timeline['base']
        timeline['foreground_round_started_ns'] = round_timing['start_wall_ns']
        timeline['foreground_round_completed_ns'] = round_timing['wall_ns']
        timeline['foreground_round_started_perf_ns'] = round_timing['start_perf_ns']
        timeline['foreground_round_completed_perf_ns'] = round_timing['perf_ns']
        with self.rpc_interval_lock:
            foreground_rpcs = [row for row in self.rpc_intervals
                               if row['thread'].startswith('soak-worker')
                               and row['start_ns'] < round_timing['perf_ns']
                               and row['end_ns'] > round_timing['start_perf_ns']]
        observed_during_rpc = [stamp for stamp in self.backup_running_observations
                               if any(row['start_ns'] <= stamp <= row['end_ns'] for row in foreground_rpcs)]
        timeline['foreground_rpc_count_during_round'] = len(foreground_rpcs)
        timeline['backup_running_observations'] = self.backup_running_observations
        timeline['observed_running_during_foreground_rpc'] = len(observed_during_rpc)
        timeline['overlap_observed'] = bool(observed_during_rpc)
        timeline['foreground_round_completed_before_durable'] = timeline['durable_ns'] >= round_timing['wall_ns']
        self.backup_timeline['base']['captured_sequence'] = durable['sequence']
        self.replay_verify(base_archive, self.root / 'base-replayed', base_revision_only=True)
        # The full backup activates replay retention. GC reports logical sweep skipped;
        # only physical reclamation can proceed, so never describe it as complete reclaim.
        if self.gc_enabled:
            status = self.drain_gc('retained-replay', allow_logical_skip=True)
            self.gc_retention_status = status
        self.phase = 'final-quiescent-backup'
        self.verify_all()
        final_archive = self.root / 'final.pack'
        self.start_backup('final', final_archive)
        final_status = self.await_backup('final')
        self.backup_timeline['final']['captured_sequence'] = final_status['sequence']
        self.replay_verify(final_archive, self.root / 'final-replayed')
        self.phase = 'complete'
        self.measure_storage('final-source')
        (self.root / 'storage-profile-final.json').write_text(
            json.dumps(json.loads(self.rpc('storage-profile')), indent=2) + '\n')
        self.stop()
        self.final_archive_bytes = final_archive.stat().st_size
        self.base_archive_bytes = base_archive.stat().st_size
        self.report('passed')

    def report(self, state, failure=None):
        self.sample_stop.set()
        if self.sample_thread:
            self.sample_thread.join(timeout=5)
        if state == 'passed':
            if self.sample_thread and self.sample_thread.is_alive():
                raise TimeoutError('storage sampler did not stop before successful report')
            self.check_background()
            self.check_artifact_budget()
        super().report(state, failure)
        path = self.root / 'summary.json'
        result = json.loads(path.read_text())
        with self.storage_peak_lock:
            sampled_peaks = dict(self.storage_peaks)
            sample_count = self.sample_count
        result.update({
            'rpc_latency_raw_summary': {k: rpc_distribution(v) for k, v in self.samples.items()},
            'storage_samples_file': str(self.root / 'storage-samples.jsonl'),
            'storage_sample_count': sample_count,
            'background_job_history': self.job_events,
            'maintenance_activity_timeline': [event for event in self.job_events
                                               if event['kind'].startswith('gc-')],
            'gc_mode': self.args.gc_mode,
            'gc_foreground_overlap': self.gc_foreground_overlap,
            'gc_active_observations': self.gc_active_observations,
            'retained_revision_counts': self.retained_revision_counts,
            'pre_backup_gc_storage': self.pre_backup_gc_storage,
            'sampled_storage_peaks': sampled_peaks,
            'sampled_storage_peaks_exact': False,
            'replayed_storage': self.replayed_storage,
            'backup_timeline': self.backup_timeline,
            'gc_retention_status': getattr(self, 'gc_retention_status', None),
            'gc_job_rates': gc_job_rates(self.job_events),
            'gc_logical_sweep_skipped': self.logical_sweep_skipped,
            'reclamation_claim': ('no GC or reclamation claim; GC was disabled'
                                  if not self.gc_enabled else
                                  'logical sweep skipped under replay retention; GC is limited to physical-only handling'
                                  if self.logical_sweep_skipped else 'no full-reclaim claim'),
            'base_archive_bytes': getattr(self, 'base_archive_bytes', None),
            'final_archive_bytes': getattr(self, 'final_archive_bytes', None),
            'scope': 'synthetic resident RPC workload; not mounted git/compiler activity',
        })
        path.write_text(json.dumps(result, indent=2))
        if state == 'passed':
            self.check_artifact_budget()

    def close(self):
        self.shutdown_phase = True
        self.sample_stop.set()
        if self.sample_thread:
            self.sample_thread.join(timeout=5)
        try:
            self.backup_monitor_stop.set()
            if self.backup_monitor_thread:
                self.backup_monitor_thread.join(timeout=5)
            self.gc_supervisor_stop.set()
            if self.gc_supervisor_thread:
                self.gc_supervisor_thread.join(timeout=5)
            if self.proc is not None and self.gc_enabled:
                # Stop GC only at final shutdown, after producers and drains have ended.
                try:
                    self.rpc('gc-stop')
                except Exception:
                    pass
            super().close()
        finally:
            self.storage_samples_log.close()
            self.job_log.close()


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
    parser.add_argument('--seconds', type=float, default=0, help='optional development-phase duration cap')
    parser.add_argument('--sample-interval', type=float, default=.5)
    parser.add_argument('--max-store-mib', type=int, default=512)
    parser.add_argument('--max-rpcs', type=int, default=1000000, help='bound retained latency samples and job observations')
    parser.add_argument('--max-artifact-mib', type=int, default=None)
    parser.add_argument('--deadline', type=float, default=1800)
    parser.add_argument('--gc-work', type=int, default=4096)
    parser.add_argument('--gc-copy-bytes', type=int, default=1024 * 1024)
    parser.add_argument('--gc-objects', type=int, default=128)
    parser.add_argument('--gc-mode', choices=('concurrent', 'disabled'), default='concurrent',
                        help='run online GC or use a same-seed no-GC foreground control')
    args = parser.parse_args()
    args.restart_every = 1
    if args.max_artifact_mib is None:
        args.max_artifact_mib = args.max_store_mib * 8
    if args.max_artifact_mib < 1:
        parser.error('max-artifact-mib must be positive')
    if not __debug__:
        parser.error('run without -O or PYTHONOPTIMIZE; the correctness oracle requires assertions')
    if args.files < 2 or args.workers < 1 or args.workers > 8 or args.rounds < 1 or args.steps < 1 \
            or args.max_store_mib < 1 or args.max_rpcs < 1:
        parser.error('files >= 2, rounds/steps positive, workers 1..8, and max-store-mib >= 1')
    if not 64 <= args.file_bytes <= 65536:
        parser.error('file-bytes must be 64..65536')
    if not math.isfinite(args.seconds) or not math.isfinite(args.sample_interval) \
            or not math.isfinite(args.deadline) or args.seconds < 0 \
            or args.sample_interval <= 0 or args.deadline <= 0:
        parser.error('seconds must be nonnegative; sample interval and deadline must be positive')
    if not 65580 <= args.gc_copy_bytes <= 8 * 1024 * 1024 or not 1 <= args.gc_objects <= 4096 \
            or not 1 <= args.gc_work <= 65536:
        parser.error('invalid GC budgets')
    trial = Soak(args)
    try:
        trial.run()
    except BaseException:
        trial.report('failed', traceback.format_exc())
        raise
    finally:
        try:
            trial.close()
        except BaseException:
            trial.report('failed', traceback.format_exc())
            raise


if __name__ == '__main__':
    main()
