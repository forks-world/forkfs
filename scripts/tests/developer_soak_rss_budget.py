"""An actual daemon must fail the sampled RSS budget with durable evidence."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

if not __debug__:
    raise RuntimeError('run without -O: this regression requires assertions')
script = Path(__file__).with_name('developer_soak.py')
with tempfile.TemporaryDirectory(prefix='ff-rss-budget-') as directory:
    output = Path(directory)
    invalid = output / 'invalid-output'
    rejected = subprocess.run([sys.executable, str(script), sys.argv[1], '--output', str(invalid),
                               '--max-rss-mib', '0'], capture_output=True, text=True, timeout=5)
    assert rejected.returncode == 2 and 'max-rss-mib must be positive' in rejected.stderr, rejected
    assert not invalid.exists(), 'invalid configuration created fixtures'
    result = subprocess.run([sys.executable, str(script), sys.argv[1], '--output', str(output),
                             '--files', '4', '--rounds', '1', '--workers', '1', '--steps', '7',
                             '--max-rss-mib', '1', '--deadline', '30'],
                            capture_output=True, text=True, timeout=45)
    assert result.returncode != 0, result.stdout
    reports = list(output.glob('seed-*/summary.json'))
    assert len(reports) == 1, (result.stdout, result.stderr)
    summary = json.loads(reports[0].read_text())
    assert summary['state'] == 'failed', summary
    assert 'daemon RSS budget exceeded' in summary['failure'], summary
    memory = summary['process_memory']
    assert memory['sample_count'] > 0 and memory['sampled_peak_rss_bytes'] > 1024 * 1024, memory
    assert memory['limit_mib'] == 1 and memory['peak_exact'] is False, memory
    samples = [json.loads(line) for line in Path(memory['samples_file']).read_text().splitlines()]
    assert any(row['rss_bytes'] > 1024 * 1024 for row in samples), samples
    for pid in {row['pid'] for row in samples}:
        probe = subprocess.run(['/bin/ps', '-p', str(pid), '-o', 'pid='], capture_output=True, text=True, timeout=2)
        assert not probe.stdout.strip(), ('daemon leaked after budget failure', pid)
