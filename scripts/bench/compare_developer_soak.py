#!/usr/bin/env python3
"""Compare matched successful developer_soak artifacts, without inventing tails."""
import argparse
import json
from pathlib import Path

MATCH = ('binary_sha256', 'harness_sha256', 'python', 'seed', 'files', 'file_bytes',
         'rounds', 'workers', 'steps', 'seconds', 'sample_interval', 'gc_work',
         'gc_copy_bytes', 'gc_objects', 'max_store_mib', 'max_artifact_mib', 'max_rpcs', 'max_rss_mib')
OPS = ('fs-save', 'fs-write-at', 'fs-append', 'fs-truncate', 'fs-rename', 'fs-chmod',
       'fs-cat', 'fs-stat', 'fs-lstat')


def compare(disabled_path, concurrent_path):
    paths = [Path(disabled_path), Path(concurrent_path)]
    summaries = [json.loads(path.read_text()) for path in paths]
    settings = [json.loads((path.parent / 'settings.json').read_text()) for path in paths]
    for result, mode in zip(summaries, ('disabled', 'concurrent')):
        if result.get('state') != 'passed' or result.get('gc_mode') != mode:
            raise ValueError(f'need a successful {mode} run')
    for key in MATCH:
        if key not in settings[0] or key not in settings[1] or settings[0][key] != settings[1][key]:
            raise ValueError(f'unmatched workload setting: {key}')
    if len(summaries[0]['retained_revision_counts']) != len(summaries[1]['retained_revision_counts']):
        raise ValueError('different actual generation counts; duration-capped traces cannot be paired')
    latency = {}
    for op in OPS:
        entries = [summary['latency_by_phase'].get(f'development/{mode}/{op}', {})
                   for summary, mode in zip(summaries, ('disabled', 'concurrent'))]
        if entries[0].get('count', 0) != entries[1].get('count', 0):
            raise ValueError(f'unmatched successful operation counts: {op}')
        if not entries[0]:
            continue
        row = {'count_per_run': entries[0]['count'], 'disabled': entries[0], 'concurrent': entries[1]}
        for q in ('p50_ms', 'p95_ms', 'p99_ms'):
            baseline, observed = entries[0].get(q), entries[1].get(q)
            row[q + '_ratio'] = observed / baseline if baseline and observed is not None else None
        latency[op] = row
    storage = {}
    for summary, mode in zip(summaries, ('disabled', 'concurrent')):
        stage = 'pre-backup-gc-after' if mode == 'concurrent' else 'development-after'
        matching = [row for row in summary['storage'] if row['stage'] == stage]
        if not matching:
            raise ValueError(f'missing development storage checkpoint: {mode}')
        storage[mode] = {'after_development': matching[-1], 'sampled_peaks': summary['sampled_storage_peaks'],
                         'pre_backup_gc_storage': summary['pre_backup_gc_storage'],
                         'base_archive_bytes': summary['base_archive_bytes'],
                         'final_archive_bytes': summary['final_archive_bytes']}
    for key in ('unique_reachable_payload_bytes', 'retained_revisions', 'modeled_worlds'):
        if storage['disabled']['after_development'][key] != storage['concurrent']['after_development'][key]:
            raise ValueError(f'unmatched retained model: {key}')
    return {'scope': 'paired synthetic RPC development phase; warm caches; sequential runs; not mounted acceptance',
            'settings': {key: settings[0][key] for key in MATCH}, 'latency': latency, 'storage': storage,
            'process_memory': {mode: result.get('process_memory') for mode, result in zip(('disabled', 'concurrent'), summaries)},
            'artifacts': [str(path.resolve()) for path in paths]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disabled', type=Path)
    parser.add_argument('concurrent', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = json.dumps(compare(args.disabled, args.concurrent), indent=2) + '\n'
    if args.output:
        with args.output.open('x') as stream:
            stream.write(result)
    else:
        print(result, end='')


if __name__ == '__main__':
    main()
