"""Bind the native periodic producer proof to exact sources and clay payloads."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

out = Path(__file__).resolve().parent
repo = out.parents[3]
prefix = sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
builds = out.parent / '2026-09-16-shared-vt-pixels'
manifest = json.loads((builds / f'{prefix}-build-manifest.json').read_text())
binary = repo / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vt_compositor_tests.exe'
fixtures = Path('/mnt/d/tmp/matter-vt/20260915-stamp-vt/faces')
raw = Path('/mnt/d/tmp/matter-vt/20260916-periodic-producer') / prefix
raw.mkdir(parents=True, exist_ok=True)
record_path = out / f'{prefix}-clay.json'
assert not record_path.exists(), 'Use a fresh evidence prefix'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def changes():
    return [p for p, h in manifest['sources'].items() if sha(repo / p) != h]


assert not changes()
binary_hash = sha(binary)
assert binary_hash == manifest['binaries'][str(binary.relative_to(repo))]
inputs = {str(fixtures / f'clay-projected-{i}.fst'): sha(fixtures / f'clay-projected-{i}.fst') for i in range(8)}
command = ('$env:MATTER_VK_VALIDATION="1"; $env:MATTER_VT_TAPE_GPU="1"; '
           '$env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP; '
           '$env:MATTER_VT_PERIODIC_CLAY_FIXTURE="D:/tmp/matter-vt/20260915-stamp-vt/faces"; '
           f'$env:MATTER_VT_PERIODIC_DUMP="D:/tmp/matter-vt/20260916-periodic-producer/{prefix}/brick-module"; '
           '& "./build/cmake/windows-msvc/relwithdebinfo/vt_compositor_tests.exe"; exit $LASTEXITCODE')
log = out / f'{prefix}-clay.log'
start = time.monotonic()
with log.open('w') as stream:
    result = subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
                             '-NoProfile', '-Command', command], cwd=repo / 'MatterEditor',
                            stdout=stream, stderr=subprocess.STDOUT)
text = log.read_text(errors='replace')
record = {'command': command, 'exit': result.returncode, 'seconds': time.monotonic() - start,
          'binary_sha256': binary_hash, 'binary_unchanged': binary_hash == sha(binary),
          'source_changes': changes(), 'input_sha256': inputs,
          'input_changes': [p for p, h in inputs.items() if sha(Path(p)) != h],
          'log_sha256': sha(log), 'all_pass': 'ALL PASS' in text,
          'zero_validation_errors': 'validation errors: 0' in text,
          'producer_metrics': [line for line in text.splitlines() if line.startswith('PERIODIC_PRODUCER ')],
          'outputs': {str(p): sha(p) for p in raw.glob('brick-module.*')}}
record['passed'] = (record['exit'] == 0 and record['all_pass'] and record['zero_validation_errors']
                    and record['binary_unchanged'] and not record['source_changes']
                    and not record['input_changes'] and len(record['producer_metrics']) == 2
                    and len(record['outputs']) == 2)
record_path.write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record), flush=True)
for line in text.splitlines():
    if re.search(r'FAIL|PERIODIC_PRODUCER|validation errors:|ALL PASS', line):
        print(line, flush=True)
sys.exit(0 if record['passed'] else 1)
