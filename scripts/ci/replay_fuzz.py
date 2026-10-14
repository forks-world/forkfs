"""Build and run bounded sanitizer/libFuzzer coverage on Linux CI."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = root/'build/replay-fuzz'
corpus = build/'corpus'
artifacts = build/'artifacts'
artifacts.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

def run(*args):
    subprocess.run(list(map(str, args)), cwd=root, env=env, check=True)

run('cmake', '-S', root, '-B', build, '-DWFS_BUILD_LEGACY=OFF', '-DWFS_COW_FSKIT=OFF',
    '-DWFS_BUILD_FUZZERS=ON', '-DCMAKE_BUILD_TYPE=Debug',
    '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++')
run('cmake', '--build', build, '--target', 'forkfsd', 'forkfs_replay_fuzzer', '--parallel', '4')
run(sys.executable, root/'daemon/tests/replay_fuzz.py', '--binary', build/'daemon/forkfsd',
    '--seed', '173', '--steps', '32', '--mutations', '0', '--corpus', corpus,
    '--artifacts', artifacts)
run(build/'daemon/forkfs_replay_fuzzer', corpus, '-max_total_time=90', '-max_len=262144',
    '-timeout=5', '-rss_limit_mb=2048', '-artifact_prefix='+str(artifacts)+'/',
    '-dict='+str(root/'daemon/tests/replay_fuzz.dict'))
