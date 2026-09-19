"""Run sequential native smoke modes against a frozen source/hash manifest."""
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

out = Path(__file__).resolve().parent
repo = out.parents[3]
prefix, *modes = sys.argv[1:]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
sources = json.loads((out / f'{prefix}-sources.json').read_text())


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def changed():
    return [p for p, h in sources.items()
            if not (repo / p).is_file() or sha(repo / p) != h]


assert not changed(), changed()
binary = repo / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe'
checks = {'binary_sha256': sha(binary), 'runs': []}
for mode in modes:
    assert re.fullmatch(r'[a-z0-9-]+', mode)
    command = '$env:MATTER_VK_VALIDATION="1"; $env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP; '
    command += f'$env:MATTER_VK_SMOKE_MODE="{mode}"; '
    command += '& "./build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe"; exit $LASTEXITCODE'
    path = out / f'{prefix}-test-{mode}.log'
    start = time.monotonic()
    with path.open('w') as log:
        result = subprocess.run([
            '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
            '-NoProfile', '-Command', command], cwd=repo / 'MatterEditor',
            stdout=log, stderr=subprocess.STDOUT)
    text = path.read_text(errors='replace')
    row = {'mode': mode, 'command': command, 'exit': result.returncode,
           'seconds': time.monotonic() - start,
           'zero_validation_errors': 'validation errors: 0' in text,
           'all_pass': 'ALL PASS' in text,
           'skipped_rt': 'ray tracing unavailable, skipping' in text,
           'log_sha256': sha(path)}
    checks['runs'].append(row)
    checks['source_changes'] = changed()
    checks['binary_unchanged'] = sha(binary) == checks['binary_sha256']
    (out / f'{prefix}-tests.json').write_text(json.dumps(checks, indent=2) + '\n')
    print(json.dumps(row), flush=True)
    for line in text.splitlines():
        if re.search(r'FAIL|Error|composed (seam eye|bend angle|diagonal eye)=|validation errors:|ALL PASS', line):
            print(line, flush=True)
    assert not checks['source_changes'] and checks['binary_unchanged']
sys.exit(1 if any(row['exit'] or not row['zero_validation_errors'] or
                 not row['all_pass'] or row['skipped_rt'] for row in checks['runs']) else 0)
